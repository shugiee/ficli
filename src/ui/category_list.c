#include "ui/category_list.h"

#include "db/query.h"
#include "models/category.h"
#include "ui/colors.h"
#include "ui/form.h"

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

    bool add_button_active = (ls->cursor == CURSOR_ADD_BUTTON && focused);
    if (add_button_active)
        wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(win, 1, 2, "[ Add Category ]");
    if (add_button_active)
        wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

    int message_row = 2;
    int header_row = 3;
    int rule_row = 4;
    int data_row_start = 5;

    if (ls->show_add_form) {
        wattron(win, A_BOLD);
        mvwprintw(win, 2, 2, "New Category");
        wattroff(win, A_BOLD);

        bool name_active = (ls->cursor == CURSOR_NAME && focused);
        if (name_active)
            wattron(win, COLOR_PAIR(COLOR_SELECTED));
        mvwprintw(win, 3, 2, "%-*.*s", field_w, field_w, "");
        mvwprintw(win, 3, 2, "%s", ls->name_buf);
        if (name_active) {
            wattroff(win, COLOR_PAIR(COLOR_SELECTED));
            curs_set(1);
            wmove(win, 3, 2 + ls->name_pos);
        } else {
            curs_set(0);
        }

        bool type_active = (ls->cursor == CURSOR_TYPE && focused);
        mvwprintw(win, 4, 2, "Type: ");
        if (type_active)
            wattron(win, COLOR_PAIR(COLOR_SELECTED));
        mvwprintw(win, 4, 8, "< %-8s >", category_type_labels[ls->type_sel]);
        if (type_active)
            wattroff(win, COLOR_PAIR(COLOR_SELECTED));

        bool submit_active = (ls->cursor == CURSOR_SUBMIT && focused);
        if (submit_active)
            wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        mvwprintw(win, 6, 2, "[ Submit ]");
        if (submit_active)
            wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

        message_row = 7;
        header_row = 8;
        rule_row = 9;
        data_row_start = 10;
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

        char line[256];
        snprintf(line, sizeof(line), " %-*s", w - 5, ls->categories[idx].name);
        mvwprintw(win, row, 2, "%s", line);

        int name_len = (int)strlen(ls->categories[idx].name);
        int type_col = 3 + name_len + 2;
        char type_tag[16];
        snprintf(type_tag, sizeof(type_tag), "[%s]",
                 category_type_labels[ls->categories[idx].type]);
        if (type_col + (int)strlen(type_tag) < w - 2) {
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
            ls->cursor = CURSOR_ADD_BUTTON;
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
            if (txn_count > 0) {
                snprintf(ls->message, sizeof(ls->message),
                         "Cannot delete: %d transaction%s linked", txn_count,
                         txn_count == 1 ? "" : "s");
                return true;
            }

            if (!confirm_delete_category(parent, category.name)) {
                snprintf(ls->message, sizeof(ls->message), "Delete cancelled");
                return true;
            }

            int rc = db_delete_category(ls->db, category.id);
            if (rc == 0) {
                ls->dirty = true;
                ls->changed = true;
                snprintf(ls->message, sizeof(ls->message), "Deleted: %.70s",
                         category.name);
            } else if (rc == -4) {
                snprintf(ls->message, sizeof(ls->message),
                         "Cannot delete: has sub-categories");
            } else if (rc == -3) {
                snprintf(ls->message, sizeof(ls->message),
                         "Cannot delete: has transactions");
            } else if (rc == -2) {
                snprintf(ls->message, sizeof(ls->message), "Category not found");
                ls->dirty = true;
                ls->changed = true;
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
            ls->cursor = CURSOR_ADD_BUTTON;
        }
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->category_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = CURSOR_ADD_BUTTON;
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
