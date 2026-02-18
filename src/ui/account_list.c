#include "ui/account_list.h"
#include "db/query.h"
#include "models/account.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Color pair IDs — must match ui.c definitions
#define COLOR_SELECTED 2

// Display labels for account types (indexes match account_type_t enum)
static const char *account_type_labels[] = {
    "Cash", "Checking", "Savings", "Credit Card", "Physical Asset", "Investment"
};

// Layout constants (rows inside box border)
// row 0: top border
// row 1: "Add Account:" label
// row 2: name input field
// row 3: type selector
// row 4: message / blank
// row 5: "Accounts" header (bold)
// row 6: horizontal rule
// data starts at row 7
#define DATA_ROW_START 7

struct account_list_state {
    sqlite3 *db;
    account_t *accounts;
    int account_count;
    int cursor;          // -1 = name input, -2 = type selector, 0+ = list items
    int scroll_offset;
    char name_buf[64];
    int name_pos;
    int type_sel;        // index into account_type_labels
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
    ls->cursor = -1; // start focused on name input
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

    // -- Name input field (row 2) --
    bool name_active = (ls->cursor == -1 && focused);
    if (name_active)
        wattron(win, COLOR_PAIR(COLOR_SELECTED));

    mvwprintw(win, 2, 2, "%-*.*s", field_w, field_w, "");
    mvwprintw(win, 2, 2, "%s", ls->name_buf);

    if (name_active) {
        wattroff(win, COLOR_PAIR(COLOR_SELECTED));
        curs_set(1);
        wmove(win, 2, 2 + ls->name_pos);
    } else {
        curs_set(0);
    }

    // -- Type selector (row 3) --
    bool type_active = (ls->cursor == -2 && focused);
    mvwprintw(win, 3, 2, "Type: ");
    if (type_active)
        wattron(win, COLOR_PAIR(COLOR_SELECTED));
    mvwprintw(win, 3, 8, "< %-16s >", account_type_labels[ls->type_sel]);
    if (type_active)
        wattroff(win, COLOR_PAIR(COLOR_SELECTED));

    // -- Message (row 4) --
    if (ls->message[0] != '\0') {
        mvwprintw(win, 4, 2, "%s", ls->message);
    }

    // -- "Accounts" header (row 5) --
    wattron(win, A_BOLD);
    mvwprintw(win, 5, 2, "Accounts");
    wattroff(win, A_BOLD);

    // -- Horizontal rule (row 6) --
    wmove(win, 6, 2);
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

        // Format: "Name  [Type]"
        char line[256];
        snprintf(line, sizeof(line), " %-*s", w - 5, ls->accounts[idx].name);
        mvwprintw(win, row, 2, "%s", line);

        if (selected) {
            wattroff(win, A_REVERSE);
            if (!focused) wattroff(win, A_DIM);
        }

        // Show type label dimmed after the name
        int name_len = (int)strlen(ls->accounts[idx].name);
        int type_col = 3 + name_len + 2; // 2=left pad, +1 for space in format, +2 gap
        const char *tlabel = account_type_labels[ls->accounts[idx].type];
        if (type_col + (int)strlen(tlabel) + 3 < w - 2) {
            wattron(win, A_DIM);
            mvwprintw(win, row, type_col, "[%s]", tlabel);
            wattroff(win, A_DIM);
        }
    }
}

bool account_list_handle_input(account_list_state_t *ls, int ch) {
    ls->message[0] = '\0';

    if (ls->cursor == -1) {
        // Name input field is focused
        if (ch == KEY_DOWN) {
            ls->cursor = -2; // move to type selector
            return true;
        }
        if (ch == '\n') {
            // Submit new account
            if (ls->name_buf[0] == '\0') {
                snprintf(ls->message, sizeof(ls->message), "Name cannot be empty");
                return true;
            }
            int64_t id = db_insert_account(ls->db, ls->name_buf,
                                           (account_type_t)ls->type_sel);
            if (id < 0) {
                snprintf(ls->message, sizeof(ls->message), "Error adding account");
            } else {
                snprintf(ls->message, sizeof(ls->message), "Added: %.56s", ls->name_buf);
                ls->name_buf[0] = '\0';
                ls->name_pos = 0;
                ls->type_sel = 0;
                ls->dirty = true;
            }
            return true;
        }
        // Text input
        handle_text_input(ls->name_buf, &ls->name_pos,
                          (int)sizeof(ls->name_buf), ch);
        return true;
    }

    if (ls->cursor == -2) {
        // Type selector is focused
        switch (ch) {
        case KEY_UP:
            ls->cursor = -1; // back to name input
            return true;
        case KEY_DOWN:
            if (ls->account_count > 0) {
                ls->cursor = 0;
                curs_set(0);
            }
            return true;
        case KEY_LEFT:
            ls->type_sel = (ls->type_sel + ACCOUNT_TYPE_COUNT - 1) % ACCOUNT_TYPE_COUNT;
            return true;
        case KEY_RIGHT:
        case '\n':
            ls->type_sel = (ls->type_sel + 1) % ACCOUNT_TYPE_COUNT;
            return true;
        default:
            return false;
        }
    }

    // List is focused
    switch (ch) {
    case KEY_UP:
    case 'k':
        if (ls->cursor > 0) {
            ls->cursor--;
        } else {
            // Move to type selector
            ls->cursor = -2;
        }
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->account_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = -1; // go to name input
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
        return "q:Quit  Enter:Add  \u2193:Type  \u2190:Sidebar";
    if (ls->cursor == -2)
        return "q:Quit  \u2190\u2192:Change Type  \u2191:Name  \u2193:List  \u2190:Sidebar";
    return "q:Quit  \u2191\u2193:Navigate  \u2190:Sidebar";
}

void account_list_mark_dirty(account_list_state_t *ls) {
    if (ls) ls->dirty = true;
}
