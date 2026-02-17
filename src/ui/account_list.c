#include "ui/account_list.h"
#include "db/query.h"
#include "models/account.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Color pair IDs — must match ui.c definitions
#define COLOR_SELECTED 2

// Layout constants (rows inside box border)
// row 0: top border
// row 1: "Add Account:" label
// row 2: name input field
// row 3: message / blank
// row 4: "Accounts" header (bold)
// row 5: horizontal rule
// data starts at row 6
#define DATA_ROW_START 6

struct account_list_state {
    sqlite3 *db;
    account_t *accounts;
    int account_count;
    int cursor;          // -1 = input field focused, 0+ = list items
    int scroll_offset;
    char name_buf[64];
    int name_pos;
    char message[64];
    bool dirty;
};

static void reload(account_list_state_t *ls) {
    free(ls->accounts);
    ls->accounts = NULL;
    ls->account_count = 0;

    ls->account_count = db_get_accounts(ls->db, &ls->accounts);
    if (ls->account_count < 0) ls->account_count = 0;

    if (ls->cursor > 0 && ls->cursor >= ls->account_count)
        ls->cursor = ls->account_count > 0 ? ls->account_count - 1 : -1;

    ls->dirty = false;
}

account_list_state_t *account_list_create(sqlite3 *db) {
    account_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls) return NULL;
    ls->db = db;
    ls->cursor = -1; // start focused on input field
    ls->dirty = true;
    return ls;
}

void account_list_destroy(account_list_state_t *ls) {
    if (!ls) return;
    free(ls->accounts);
    free(ls);
}

static void handle_text_input(char *buf, int *pos, int maxlen, int ch) {
    int len = (int)strlen(buf);

    if (ch == KEY_LEFT) {
        if (*pos > 0) (*pos)--;
    } else if (ch == KEY_RIGHT) {
        if (*pos < len) (*pos)++;
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

void account_list_draw(account_list_state_t *ls, WINDOW *win, bool focused) {
    if (ls->dirty) reload(ls);

    int h, w;
    getmaxyx(win, h, w);

    int field_w = w - 6; // padding on each side
    if (field_w < 10) field_w = 10;
    if (field_w > 60) field_w = 60;

    // -- "Add Account:" label (row 1) --
    wattron(win, A_BOLD);
    mvwprintw(win, 1, 2, "Add Account:");
    wattroff(win, A_BOLD);

    // -- Input field (row 2) --
    bool input_active = (ls->cursor == -1 && focused);
    if (input_active)
        wattron(win, COLOR_PAIR(COLOR_SELECTED));

    mvwprintw(win, 2, 2, "%-*.*s", field_w, field_w, "");
    mvwprintw(win, 2, 2, "%s", ls->name_buf);

    if (input_active) {
        wattroff(win, COLOR_PAIR(COLOR_SELECTED));
        curs_set(1);
        wmove(win, 2, 2 + ls->name_pos);
    } else {
        curs_set(0);
    }

    // -- Message (row 3) --
    if (ls->message[0] != '\0') {
        mvwprintw(win, 3, 2, "%s", ls->message);
    }

    // -- "Accounts" header (row 4) --
    wattron(win, A_BOLD);
    mvwprintw(win, 4, 2, "Accounts");
    wattroff(win, A_BOLD);

    // -- Horizontal rule (row 5) --
    wmove(win, 5, 2);
    for (int i = 2; i < w - 2; i++)
        waddch(win, ACS_HLINE);

    // -- Account list --
    int visible_rows = h - 1 - DATA_ROW_START;
    if (visible_rows < 1) visible_rows = 1;

    if (ls->account_count == 0) {
        const char *msg = "No accounts";
        int mlen = (int)strlen(msg);
        int row = DATA_ROW_START + visible_rows / 2;
        if (row >= h - 1) row = DATA_ROW_START;
        mvwprintw(win, row, (w - mlen) / 2, "%s", msg);
        return;
    }

    // Clamp cursor/scroll for list
    if (ls->cursor >= ls->account_count)
        ls->cursor = ls->account_count - 1;

    if (ls->cursor >= 0) {
        if (ls->cursor < ls->scroll_offset)
            ls->scroll_offset = ls->cursor;
        if (ls->cursor >= ls->scroll_offset + visible_rows)
            ls->scroll_offset = ls->cursor - visible_rows + 1;
    }
    if (ls->scroll_offset < 0) ls->scroll_offset = 0;

    for (int i = 0; i < visible_rows; i++) {
        int idx = ls->scroll_offset + i;
        if (idx >= ls->account_count) break;

        int row = DATA_ROW_START + i;
        bool selected = (idx == ls->cursor);

        if (selected) {
            if (!focused) wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        mvwprintw(win, row, 2, " %-*s", w - 5, ls->accounts[idx].name);

        if (selected) {
            wattroff(win, A_REVERSE);
            if (!focused) wattroff(win, A_DIM);
        }
    }
}

bool account_list_handle_input(account_list_state_t *ls, int ch) {
    ls->message[0] = '\0';

    if (ls->cursor == -1) {
        // Input field is focused
        if (ch == KEY_DOWN) {
            if (ls->account_count > 0) {
                ls->cursor = 0;
                curs_set(0);
            }
            return true;
        }
        if (ch == '\n') {
            // Submit new account
            if (ls->name_buf[0] == '\0') {
                snprintf(ls->message, sizeof(ls->message), "Name cannot be empty");
                return true;
            }
            int64_t id = db_insert_account(ls->db, ls->name_buf);
            if (id < 0) {
                snprintf(ls->message, sizeof(ls->message), "Error adding account");
            } else {
                snprintf(ls->message, sizeof(ls->message), "Added: %.56s", ls->name_buf);
                ls->name_buf[0] = '\0';
                ls->name_pos = 0;
                ls->dirty = true;
            }
            return true;
        }
        // Text input
        handle_text_input(ls->name_buf, &ls->name_pos,
                          (int)sizeof(ls->name_buf), ch);
        return true;
    }

    // List is focused
    switch (ch) {
    case KEY_UP:
    case 'k':
        if (ls->cursor > 0) {
            ls->cursor--;
        } else {
            // Move to input field
            ls->cursor = -1;
        }
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->account_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = -1; // go to input field
        return true;
    case KEY_END:
    case 'G':
        ls->cursor = ls->account_count > 0 ? ls->account_count - 1 : 0;
        return true;
    default:
        return false;
    }
}

const char *account_list_status_hint(const account_list_state_t *ls) {
    if (ls->cursor == -1)
        return "q:Quit  Enter:Add  \u2193:List  \u2190:Sidebar";
    return "q:Quit  \u2191\u2193:Navigate  \u2190:Sidebar";
}

void account_list_mark_dirty(account_list_state_t *ls) {
    if (ls) ls->dirty = true;
}
