#include "ui/category_list.h"

#include "db/query.h"
#include "models/category.h"
#include "ui/colors.h"
#include "ui/form.h"
#include "ui/resize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *category_type_labels[] = {"Expense", "Income"};

#define CURSOR_NAME -1
#define CURSOR_TYPE -2
#define CURSOR_SUBMIT -3
#define CURSOR_ADD_BUTTON -4

struct category_list_state {
    sqlite3 *db;
    category_t *categories;
    int category_count;
    int cursor; // negative = controls, 0+ = list
    int scroll_offset;
    bool show_add_form;
    char name_buf[64];
    int name_pos;
    category_type_t type_sel;
    char message[96];
    bool dirty;
    bool changed;
};

typedef struct {
    int64_t id;
    char name[64];
} delete_reassign_option_t;

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

static bool confirm_delete_category(WINDOW *parent, const char *category_name) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = 7;
    int win_w = 56;
    if (ph < win_h)
        win_h = ph;
    if (pw < win_w)
        win_w = pw;
    if (win_h < 5 || win_w < 30)
        return false;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    mvwprintw(w, 1, 2, "Delete category '%-.36s'?", category_name);
    mvwprintw(w, 3, 2, "This action cannot be undone.");
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

static int choose_delete_reassignment(WINDOW *parent, sqlite3 *db,
                                      const category_t *category, int txn_count,
                                      int64_t *out_category_id,
                                      char *out_category_name,
                                      size_t out_category_name_sz) {
    if (!parent || !db || !category || !out_category_id)
        return -1;

    category_t *same_type = NULL;
    int same_type_count = db_get_categories(db, category->type, &same_type);
    if (same_type_count < 0)
        return -1;

    int option_count = 1;
    for (int i = 0; i < same_type_count; i++) {
        if (same_type[i].id != category->id)
            option_count++;
    }

    delete_reassign_option_t *options =
        calloc((size_t)option_count, sizeof(*options));
    if (!options) {
        free(same_type);
        return -1;
    }

    int idx = 0;
    options[idx].id = 0;
    snprintf(options[idx].name, sizeof(options[idx].name), "Uncategorized");
    idx++;
    for (int i = 0; i < same_type_count; i++) {
        if (same_type[i].id == category->id)
            continue;
        options[idx].id = same_type[i].id;
        snprintf(options[idx].name, sizeof(options[idx].name), "%s",
                 same_type[i].name);
        idx++;
    }
    free(same_type);

    int ph, pw;
    getmaxyx(parent, ph, pw);

    int visible = option_count < 6 ? option_count : 6;
    int win_h = 8 + visible;
    int win_w = 68;
    if (ph < win_h)
        win_h = ph;
    if (pw < win_w)
        win_w = pw;
    if (win_h < 10 || win_w < 38) {
        free(options);
        return 0;
    }

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    if (!w) {
        free(options);
        return -1;
    }
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));

    int sel = 0;
    int scroll = 0;
    int done = 0;
    int result = 0;

    while (!done) {
        werase(w);
        box(w, 0, 0);

        mvwprintw(w, 1, 2, "Delete '%-.30s' with %d linked transaction%s",
                  category->name, txn_count, txn_count == 1 ? "" : "s");
        mvwprintw(w, 2, 2, "Reassign linked transactions to:");

        if (sel < scroll)
            scroll = sel;
        if (sel >= scroll + visible)
            scroll = sel - visible + 1;
        if (scroll < 0)
            scroll = 0;

        int list_row = 4;
        int list_w = win_w - 4;
        if (list_w < 1)
            list_w = 1;
        for (int i = 0; i < visible; i++) {
            int opt_idx = scroll + i;
            if (opt_idx >= option_count)
                break;

            bool selected = (opt_idx == sel);
            if (selected)
                wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
            mvwprintw(w, list_row + i, 2, "%-*s", list_w, "");
            mvwprintw(w, list_row + i, 2, "%.*s", list_w, options[opt_idx].name);
            if (selected)
                wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        }
        if (scroll > 0)
            mvwaddch(w, list_row, win_w - 2, ACS_UARROW);
        if (scroll + visible < option_count)
            mvwaddch(w, list_row + visible - 1, win_w - 2, ACS_DARROW);

        mvwprintw(w, win_h - 2, 2, "Enter:Delete  Esc:Cancel  \u2191\u2193 choose");
        wrefresh(w);

        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            result = 0;
            done = 1;
            continue;
        }
        switch (ch) {
        case KEY_UP:
        case 'k':
            if (sel > 0)
                sel--;
            break;
        case KEY_DOWN:
        case 'j':
            if (sel < option_count - 1)
                sel++;
            break;
        case '\n':
            *out_category_id = options[sel].id;
            if (out_category_name && out_category_name_sz > 0) {
                snprintf(out_category_name, out_category_name_sz, "%s",
                         options[sel].name);
            }
            result = 1;
            done = 1;
            break;
        case 27:
            result = 0;
            done = 1;
            break;
        default:
            break;
        }
    }

    delwin(w);
    free(options);
    touchwin(parent);
    return result;
}

static void reload(category_list_state_t *ls) {
    free(ls->categories);
    ls->categories = NULL;
    ls->category_count = 0;

    category_t *expense = NULL;
    category_t *income = NULL;
    int exp_count = db_get_categories(ls->db, CATEGORY_EXPENSE, &expense);
    int inc_count = db_get_categories(ls->db, CATEGORY_INCOME, &income);
    if (exp_count < 0)
        exp_count = 0;
    if (inc_count < 0)
        inc_count = 0;

    int total = exp_count + inc_count;
    if (total > 0) {
        ls->categories = calloc((size_t)total, sizeof(category_t));
        if (ls->categories) {
            if (exp_count > 0)
                memcpy(ls->categories, expense, (size_t)exp_count * sizeof(category_t));
            if (inc_count > 0) {
                memcpy(ls->categories + exp_count, income,
                       (size_t)inc_count * sizeof(category_t));
            }
            ls->category_count = total;
        }
    }

    free(expense);
    free(income);

    if (ls->cursor >= 0 && ls->cursor >= ls->category_count)
        ls->cursor =
            ls->category_count > 0 ? ls->category_count - 1 : CURSOR_ADD_BUTTON;

    ls->dirty = false;
}

category_list_state_t *category_list_create(sqlite3 *db) {
    category_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    ls->cursor = CURSOR_ADD_BUTTON;
    ls->type_sel = CATEGORY_EXPENSE;
    ls->dirty = true;
    return ls;
}

void category_list_destroy(category_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->categories);
    free(ls);
}

static void handle_text_input(char *buf, int *pos, int maxlen, int ch) {
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
        memmove(&buf[*pos + 1], &buf[*pos], len - *pos + 1);
        buf[*pos] = (char)ch;
        (*pos)++;
    }
}

static void draw_box(WINDOW *win, int top, int left, int bottom, int right) {
    if (!win || top >= bottom || left >= right)
        return;
    mvwhline(win, top, left + 1, ACS_HLINE, right - left - 1);
    mvwhline(win, bottom, left + 1, ACS_HLINE, right - left - 1);
    mvwvline(win, top + 1, left, ACS_VLINE, bottom - top - 1);
    mvwvline(win, top + 1, right, ACS_VLINE, bottom - top - 1);
    mvwaddch(win, top, left, ACS_ULCORNER);
    mvwaddch(win, top, right, ACS_URCORNER);
    mvwaddch(win, bottom, left, ACS_LLCORNER);
    mvwaddch(win, bottom, right, ACS_LRCORNER);
}

void category_list_draw(category_list_state_t *ls, WINDOW *win, bool focused) {
    if (ls->dirty)
        reload(ls);

    int h, w;
    getmaxyx(win, h, w);

    int field_w = w - 6;
    if (field_w < 10)
        field_w = 10;
    if (field_w > 60)
        field_w = 60;

    if (!ls->show_add_form) {
        bool add_button_active = (ls->cursor == CURSOR_ADD_BUTTON && focused);
        if (add_button_active)
            wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        mvwprintw(win, 1, 2, "[ Add Category ]");
        if (add_button_active)
            wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    }

    int message_row = 2;
    int header_row = 3;
    int rule_row = 4;
    int data_row_start = 5;

    if (ls->show_add_form) {
        int form_top = 2;
        int form_left_col = (w >= 56) ? 4 : 2;
        int form_right_col = w - 3;
        if (form_right_col - form_left_col + 1 > 56)
            form_right_col = form_left_col + 55;
        if (form_right_col > w - 2)
            form_right_col = w - 2;
        if (form_right_col - form_left_col + 1 < 24) {
            form_left_col = 2;
            form_right_col = w - 2;
        }

        int form_label_col = form_left_col + 2;
        int form_field_col = form_label_col + 13;
        if (form_field_col > form_right_col - 9)
            form_field_col = form_right_col - 9;
        if (form_field_col < form_label_col + 7)
            form_field_col = form_label_col + 7;
        int form_field_w = form_right_col - form_field_col - 1;
        if (form_field_w < 1)
            form_field_w = 1;
        if (form_field_w > 36)
            form_field_w = 36;

        int name_row = form_top + 1;
        int type_row = name_row + 1;
        int submit_row = type_row + 2;
        int form_bottom = submit_row;

        draw_box(win, form_top, form_left_col, form_bottom, form_right_col);

        wattron(win, A_BOLD);
        mvwprintw(win, form_top, form_left_col + 2, " Add Category ");
        wattroff(win, A_BOLD);

        bool name_active = (ls->cursor == CURSOR_NAME && focused);
        if (name_active)
            wattron(win, COLOR_PAIR(COLOR_INFO) | A_BOLD);
        mvwprintw(win, name_row, form_label_col, "Name:");
        if (name_active)
            wattroff(win, COLOR_PAIR(COLOR_INFO) | A_BOLD);
        int name_field_color =
            COLOR_PAIR(name_active ? COLOR_FORM_ACTIVE : COLOR_FORM_DROPDOWN);
        wattron(win, name_field_color);
        mvwprintw(win, name_row, form_field_col, "%-*s", form_field_w, "");
        mvwprintw(win, name_row, form_field_col, "%.*s", form_field_w, ls->name_buf);
        wattroff(win, name_field_color);
        if (name_active) {
            curs_set(1);
            int name_cursor_col = form_field_col + ls->name_pos;
            if (name_cursor_col > form_field_col + form_field_w - 1)
                name_cursor_col = form_field_col + form_field_w - 1;
            wmove(win, name_row, name_cursor_col);
        } else {
            curs_set(0);
        }

        bool type_active = (ls->cursor == CURSOR_TYPE && focused);
        if (type_active)
            wattron(win, COLOR_PAIR(COLOR_INFO) | A_BOLD);
        mvwprintw(win, type_row, form_label_col, "Type:");
        if (type_active)
            wattroff(win, COLOR_PAIR(COLOR_INFO) | A_BOLD);
        int type_field_color =
            COLOR_PAIR(type_active ? COLOR_FORM_ACTIVE : COLOR_FORM_DROPDOWN);
        wattron(win, type_field_color);
        mvwprintw(win, type_row, form_field_col, "%-*s", form_field_w, "");
        mvwprintw(win, type_row, form_field_col, "< %-8s >",
                  category_type_labels[ls->type_sel]);
        wattroff(win, type_field_color);

        bool submit_active = (ls->cursor == CURSOR_SUBMIT && focused);
        const char *submit_label = "[ Submit ]";
        int submit_col = form_right_col - (int)strlen(submit_label) - 2;
        if (submit_col < form_left_col + 2)
            submit_col = form_left_col + 2;
        if (submit_active)
            wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        mvwprintw(win, submit_row, submit_col, "%s", submit_label);
        if (submit_active)
            wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

        message_row = form_bottom + 1;
        header_row = message_row + 1;
        rule_row = header_row + 1;
        data_row_start = rule_row + 1;
    } else {
        curs_set(0);
    }

    if (ls->message[0] != '\0')
        mvwprintw(win, message_row, 2, "%s", ls->message);
    else
        mvwprintw(win, message_row, 2, "%-*s", field_w + 10, "");

    wattron(win, A_BOLD);
    mvwprintw(win, header_row, 2, "Categories");
    wattroff(win, A_BOLD);

    wmove(win, rule_row, 2);
    for (int i = 2; i < w - 2; i++)
        waddch(win, ACS_HLINE);

    int visible_rows = h - 1 - data_row_start;
    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->category_count == 0) {
        const char *msg = "No categories";
        int mlen = (int)strlen(msg);
        int row = data_row_start + visible_rows / 2;
        if (row >= h - 1)
            row = data_row_start;
        mvwprintw(win, row, (w - mlen) / 2, "%s", msg);
        return;
    }

    if (ls->cursor >= ls->category_count)
        ls->cursor = ls->category_count > 0 ? ls->category_count - 1
                                             : CURSOR_ADD_BUTTON;
    if (ls->cursor >= 0) {
        if (ls->cursor < ls->scroll_offset)
            ls->scroll_offset = ls->cursor;
        if (ls->cursor >= ls->scroll_offset + visible_rows)
            ls->scroll_offset = ls->cursor - visible_rows + 1;
    }
    if (ls->scroll_offset < 0)
        ls->scroll_offset = 0;

    int left_col = 2;
    int right_col = w - 2;
    int gap = 2;
    int max_name_len = 0;
    int max_type_len = 0;
    for (int i = 0; i < ls->category_count; i++) {
        int name_len = (int)strlen(ls->categories[i].name);
        if (name_len > max_name_len)
            max_name_len = name_len;

        char type_tag[16];
        snprintf(type_tag, sizeof(type_tag), "[%s]",
                 category_type_labels[ls->categories[i].type]);
        int type_len = (int)strlen(type_tag);
        if (type_len > max_type_len)
            max_type_len = type_len;
    }

    int type_col = left_col + max_name_len + gap;
    int max_type_col = right_col - max_type_len;
    if (type_col > max_type_col)
        type_col = max_type_col;
    if (type_col < left_col + gap + 1)
        type_col = left_col + gap + 1;
    int name_w = type_col - left_col - gap;
    if (name_w < 1)
        name_w = 1;

    for (int i = 0; i < visible_rows; i++) {
        int idx = ls->scroll_offset + i;
        if (idx >= ls->category_count)
            break;

        int row = data_row_start + i;
        bool selected = (idx == ls->cursor);
        if (selected) {
            if (!focused)
                wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        mvwprintw(win, row, left_col, "%-*.*s", right_col - left_col, right_col - left_col, "");
        mvwprintw(win, row, left_col, "%-*.*s", name_w, name_w,
                  ls->categories[idx].name);

        char type_tag[16];
        snprintf(type_tag, sizeof(type_tag), "[%s]",
                 category_type_labels[ls->categories[idx].type]);
        if (type_col + (int)strlen(type_tag) < right_col) {
            wattron(win, A_DIM);
            mvwprintw(win, row, type_col, "%s", type_tag);
            wattroff(win, A_DIM);
        }

        if (selected) {
            wattroff(win, A_REVERSE);
            if (!focused)
                wattroff(win, A_DIM);
        }
    }
}

static bool submit_category(category_list_state_t *ls) {
    char parent_name[64];
    char child_name[64];
    bool has_parent = false;

    if (!parse_category_path(ls->name_buf, parent_name, sizeof(parent_name),
                             child_name, sizeof(child_name), &has_parent)) {
        snprintf(ls->message, sizeof(ls->message), "Invalid category path");
        return false;
    }

    int64_t parent_id = 0;
    if (has_parent) {
        parent_id = db_get_or_create_category(ls->db, ls->type_sel, parent_name, 0);
        if (parent_id <= 0) {
            snprintf(ls->message, sizeof(ls->message), "Error adding category");
            return false;
        }
    }

    int64_t id = db_get_or_create_category(ls->db, ls->type_sel, child_name,
                                           parent_id);
    if (id <= 0) {
        snprintf(ls->message, sizeof(ls->message), "Error adding category");
        return false;
    } else {
        snprintf(ls->message, sizeof(ls->message), "Saved: %.72s", ls->name_buf);
        ls->name_buf[0] = '\0';
        ls->name_pos = 0;
        ls->type_sel = CATEGORY_EXPENSE;
        ls->dirty = true;
        ls->changed = true;
        return true;
    }
}

bool category_list_handle_input(category_list_state_t *ls, WINDOW *parent, int ch) {
    ls->message[0] = '\0';

    if (!ls->show_add_form && ls->cursor == CURSOR_ADD_BUTTON) {
        switch (ch) {
        case '\n':
            ls->show_add_form = true;
            ls->cursor = CURSOR_NAME;
            return true;
        case KEY_DOWN:
        case 'j':
            if (ls->category_count > 0)
                ls->cursor = 0;
            return true;
        default:
            return false;
        }
    }

    if (ls->show_add_form && ls->cursor == CURSOR_NAME) {
        if (ch == 27) {
            ls->show_add_form = false;
            ls->cursor = CURSOR_ADD_BUTTON;
            return true;
        }
        if (ch == KEY_UP || ch == 'k') {
            return true;
        }
        if (ch == KEY_DOWN || ch == '\n') {
            ls->cursor = CURSOR_TYPE;
            return true;
        }
        handle_text_input(ls->name_buf, &ls->name_pos, (int)sizeof(ls->name_buf),
                          ch);
        return true;
    }

    if (ls->show_add_form && ls->cursor == CURSOR_TYPE) {
        switch (ch) {
        case 27:
            ls->show_add_form = false;
            ls->cursor = CURSOR_ADD_BUTTON;
            return true;
        case KEY_UP:
        case 'k':
            ls->cursor = CURSOR_NAME;
            return true;
        case KEY_DOWN:
        case 'j':
        case '\n':
            ls->cursor = CURSOR_SUBMIT;
            return true;
        case KEY_LEFT:
        case 'h':
            ls->type_sel = (ls->type_sel == CATEGORY_EXPENSE) ? CATEGORY_INCOME
                                                               : CATEGORY_EXPENSE;
            return true;
        case KEY_RIGHT:
        case 'l':
            ls->type_sel = (ls->type_sel == CATEGORY_EXPENSE) ? CATEGORY_INCOME
                                                               : CATEGORY_EXPENSE;
            return true;
        default:
            return false;
        }
    }

    if (ls->show_add_form && ls->cursor == CURSOR_SUBMIT) {
        switch (ch) {
        case 27:
            ls->show_add_form = false;
            ls->cursor = CURSOR_ADD_BUTTON;
            return true;
        case KEY_UP:
        case 'k':
            ls->cursor = CURSOR_TYPE;
            return true;
        case KEY_DOWN:
        case 'j':
            if (ls->category_count > 0)
                ls->cursor = 0;
            return true;
        case '\n':
            if (submit_category(ls)) {
                ls->show_add_form = false;
                ls->cursor = CURSOR_ADD_BUTTON;
            }
            return true;
        default:
            return false;
        }
    }

    switch (ch) {
    case 'e':
        if (ls->cursor >= 0 && ls->cursor < ls->category_count) {
            category_t category = ls->categories[ls->cursor];
            form_result_t res = form_category(parent, ls->db, &category, true);
            if (res == FORM_SAVED) {
                ls->dirty = true;
                ls->changed = true;
                snprintf(ls->message, sizeof(ls->message), "Updated: %.70s",
                         category.name);
            }
        }
        return true;
    case 'd':
        if (ls->cursor >= 0 && ls->cursor < ls->category_count) {
            category_t category = ls->categories[ls->cursor];
            int child_count = db_count_child_categories(ls->db, category.id);
            int txn_count = db_count_transactions_for_category(ls->db, category.id);
            if (child_count < 0 || txn_count < 0) {
                snprintf(ls->message, sizeof(ls->message), "Error checking category");
                return true;
            }
            if (child_count > 0) {
                snprintf(ls->message, sizeof(ls->message),
                         "Cannot delete: %d sub-categor%s exist", child_count,
                         child_count == 1 ? "y" : "ies");
                return true;
            }

            int64_t replacement_category_id = 0;
            char replacement_category_name[64];
            snprintf(replacement_category_name, sizeof(replacement_category_name),
                     "Uncategorized");

            if (txn_count > 0) {
                int choose_rc = choose_delete_reassignment(
                    parent, ls->db, &category, txn_count, &replacement_category_id,
                    replacement_category_name, sizeof(replacement_category_name));
                if (choose_rc < 0) {
                    snprintf(ls->message, sizeof(ls->message),
                             "Error loading category choices");
                    return true;
                }
                if (choose_rc == 0) {
                    snprintf(ls->message, sizeof(ls->message), "Delete cancelled");
                    return true;
                }
            } else if (!confirm_delete_category(parent, category.name)) {
                snprintf(ls->message, sizeof(ls->message), "Delete cancelled");
                return true;
            }

            int rc = db_delete_category_with_reassignment(ls->db, category.id,
                                                          replacement_category_id);
            if (rc == 0) {
                ls->dirty = true;
                ls->changed = true;
                if (txn_count > 0) {
                    snprintf(ls->message, sizeof(ls->message),
                             "Deleted: %.22s (%d txn%s -> %.40s)", category.name,
                             txn_count, txn_count == 1 ? "" : "s",
                             replacement_category_name);
                } else {
                    snprintf(ls->message, sizeof(ls->message), "Deleted: %.70s",
                             category.name);
                }
            } else if (rc == -4) {
                snprintf(ls->message, sizeof(ls->message),
                         "Cannot delete: has sub-categories");
            } else if (rc == -2) {
                snprintf(ls->message, sizeof(ls->message), "Category not found");
                ls->dirty = true;
                ls->changed = true;
            } else if (rc == -5) {
                snprintf(ls->message, sizeof(ls->message),
                         "Invalid replacement category");
            } else {
                snprintf(ls->message, sizeof(ls->message), "Error deleting category");
            }
        }
        return true;
    case KEY_UP:
    case 'k':
        if (ls->cursor > 0) {
            ls->cursor--;
        } else {
            ls->cursor = ls->show_add_form ? CURSOR_SUBMIT : CURSOR_ADD_BUTTON;
        }
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->category_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = ls->show_add_form ? CURSOR_NAME : CURSOR_ADD_BUTTON;
        return true;
    case KEY_END:
    case 'G':
        ls->cursor = ls->category_count > 0 ? ls->category_count - 1
                                            : CURSOR_ADD_BUTTON;
        return true;
    default:
        return false;
    }
}

const char *category_list_status_hint(const category_list_state_t *ls) {
    if (ls->cursor == CURSOR_ADD_BUTTON)
        return "q:Quit  Enter:Show Add Form  \u2193:List  \u2190:Sidebar";
    if (ls->cursor == CURSOR_NAME)
        return "q:Quit  Enter:Next  Type Parent:Child  Esc:Close Form  \u2190:Sidebar";
    if (ls->cursor == CURSOR_TYPE)
        return "q:Quit  \u2190\u2192:Change Type  \u2191:Name  \u2193:Submit  Esc:Close Form";
    if (ls->cursor == CURSOR_SUBMIT)
        return "q:Quit  Enter:Submit  \u2191:Type  \u2193:List  Esc:Close Form";
    return "q:Quit  \u2191\u2193:Navigate  e:Edit  d:Delete  \u2190:Sidebar";
}

void category_list_mark_dirty(category_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}

bool category_list_consume_changed(category_list_state_t *ls) {
    if (!ls || !ls->changed)
        return false;
    ls->changed = false;
    return true;
}
