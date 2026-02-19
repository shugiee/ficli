#include "ui/account_list.h"
#include "db/query.h"
#include "models/account.h"
#include "ui/colors.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Display labels for account types (indexes match account_type_t enum)
static const char *account_type_labels[] = {"Cash",           "Checking",
                                            "Savings",        "Credit Card",
                                            "Physical Asset", "Investment"};

// Layout constants (rows inside box border)
// row 0: top border
// row 1: "Add Account:" label
// row 2: name input field
// row 3: type selector
// row 4: card last 4 (only shown for Credit Card type)
// row 5: message / blank
// row 6: "Accounts" header (bold)
// row 7: horizontal rule
// data starts at row 8
#define DATA_ROW_START 8

struct account_list_state {
    sqlite3 *db;
    account_t *accounts;
    int account_count;
    int cursor; // -1 = name input, -2 = type selector, -3 = card last 4, 0+ =
                // list items
    int scroll_offset;
    char name_buf[64];
    int name_pos;
    int type_sel;           // index into account_type_labels
    char card_last4_buf[5]; // up to 4 digits
    int card_last4_pos;
    char message[64];
    bool dirty;
    bool account_added;
};

static void reload(account_list_state_t *ls) {
    free(ls->accounts);
    ls->accounts = NULL;
    ls->account_count = 0;

    ls->account_count = db_get_accounts(ls->db, &ls->accounts);
    if (ls->account_count < 0)
        ls->account_count = 0;

    if (ls->cursor > 0 && ls->cursor >= ls->account_count)
        ls->cursor = ls->account_count > 0 ? ls->account_count - 1 : -1;

    ls->dirty = false;
}

account_list_state_t *account_list_create(sqlite3 *db) {
    account_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    ls->cursor = -1; // start focused on name input
    ls->dirty = true;
    return ls;
}

void account_list_destroy(account_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->accounts);
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

void account_list_draw(account_list_state_t *ls, WINDOW *win, bool focused) {
    if (ls->dirty)
        reload(ls);

    int h, w;
    getmaxyx(win, h, w);

    int field_w = w - 6; // padding on each side
    if (field_w < 10)
        field_w = 10;
    if (field_w > 60)
        field_w = 60;

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

    // -- Card last 4 (row 4, credit card only) --
    bool card_active = (ls->cursor == -3 && focused);
    if (ls->type_sel == ACCOUNT_CREDIT_CARD) {
        mvwprintw(win, 4, 2, "Card last 4: ");
        if (card_active)
            wattron(win, COLOR_PAIR(COLOR_SELECTED));
        mvwprintw(win, 4, 15, "%-4s", ls->card_last4_buf);
        if (card_active) {
            wattroff(win, COLOR_PAIR(COLOR_SELECTED));
            curs_set(1);
            wmove(win, 4, 15 + ls->card_last4_pos);
        }
    } else {
        mvwprintw(win, 4, 2, "%-*s", field_w + 10, ""); // clear row
    }

    // -- Type selector (row 4) --
    bool submit_button_focused = (ls->cursor == -4 && focused);
    if (submit_button_focused)
        wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(win, 5, 2, "[ Submit ]");
    if (submit_button_focused)
        wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

    // -- Message (row 6) --
    if (ls->message[0] != '\0') {
        mvwprintw(win, 6, 2, "%s", ls->message);
    } else {
        mvwprintw(win, 6, 2, "%-*s", field_w + 10, ""); // clear row
    }

    // -- "Accounts" header (row 6) --
    wattron(win, A_BOLD);
    mvwprintw(win, 7, 2, "Accounts");
    wattroff(win, A_BOLD);

    // -- Horizontal rule (row 7) --
    wmove(win, 8, 2);
    for (int i = 2; i < w - 2; i++)
        waddch(win, ACS_HLINE);

    // -- Account list --
    int visible_rows = h - 1 - DATA_ROW_START;
    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->account_count == 0) {
        const char *msg = "No accounts";
        int mlen = (int)strlen(msg);
        int row = DATA_ROW_START + visible_rows / 2;
        if (row >= h - 1)
            row = DATA_ROW_START;
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
    if (ls->scroll_offset < 0)
        ls->scroll_offset = 0;

    for (int i = 0; i < visible_rows; i++) {
        int idx = ls->scroll_offset + i;
        if (idx >= ls->account_count)
            break;

        int row = DATA_ROW_START + i;
        bool selected = (idx == ls->cursor);

        if (selected) {
            if (!focused)
                wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        // Format: "Name  [Type]"
        char line[256];
        snprintf(line, sizeof(line), " %-*s", w - 5, ls->accounts[idx].name);
        mvwprintw(win, row, 2, "%s", line);

        // Show type label (and card last 4 for credit cards) dimmed after name
        int name_len = (int)strlen(ls->accounts[idx].name);
        int type_col = 3 + name_len + 2;
        const char *tlabel = account_type_labels[ls->accounts[idx].type];
        char type_tag[32];
        if (ls->accounts[idx].type == ACCOUNT_CREDIT_CARD &&
            ls->accounts[idx].card_last4[0] != '\0') {
            snprintf(type_tag, sizeof(type_tag), "[%s ****%s]", tlabel,
                     ls->accounts[idx].card_last4);
        } else {
            snprintf(type_tag, sizeof(type_tag), "[%s]", tlabel);
        }
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

static void submit_account(account_list_state_t *ls) {
    if (ls->name_buf[0] == '\0') {
        snprintf(ls->message, sizeof(ls->message), "Name cannot be empty");
        return;
    }
    const char *card =
        (ls->type_sel == ACCOUNT_CREDIT_CARD) ? ls->card_last4_buf : NULL;
    int64_t id = db_insert_account(ls->db, ls->name_buf,
                                   (account_type_t)ls->type_sel, card);
    if (id < 0) {
        snprintf(ls->message, sizeof(ls->message), "Error adding account");
    } else {
        snprintf(ls->message, sizeof(ls->message), "Added: %.56s",
                 ls->name_buf);
        ls->name_buf[0] = '\0';
        ls->name_pos = 0;
        ls->type_sel = 0;
        ls->card_last4_buf[0] = '\0';
        ls->card_last4_pos = 0;
        ls->dirty = true;
        ls->account_added = true;
    }
}

bool account_list_handle_input(account_list_state_t *ls, int ch) {
    ls->message[0] = '\0';

    // Name input field is focused
    if (ls->cursor == -1) {
        if (ch == KEY_DOWN || ch == '\n') {
            ls->cursor = -2; // move to type selector
            return true;
        }
        // Text input
        handle_text_input(ls->name_buf, &ls->name_pos,
                          (int)sizeof(ls->name_buf), ch);
        return true;
    }

    // Type selector is focused
    if (ls->cursor == -2) {
        switch (ch) {
        case KEY_UP:
        case 'k':
            ls->cursor = -1; // back to name input
            return true;
        case KEY_DOWN:
        case 'j':
        case '\n':
            if (ls->type_sel == ACCOUNT_CREDIT_CARD) {
                ls->cursor = -3; // go to card last 4
            } else {
                // Go to submit button
                ls->cursor = -4;
            }
            return true;
        case KEY_LEFT:
        case 'h':
            ls->type_sel =
                (ls->type_sel + ACCOUNT_TYPE_COUNT - 1) % ACCOUNT_TYPE_COUNT;
            return true;
        case KEY_RIGHT:
        case 'l':
            ls->type_sel = (ls->type_sel + 1) % ACCOUNT_TYPE_COUNT;
            return true;
        default:
            return false;
        }
    }

    // Card last 4 input is focused
    if (ls->cursor == -3) {
        switch (ch) {
        case KEY_UP:
        case 'k':
            ls->cursor = -2; // back to type selector
            return true;
        case KEY_DOWN:
        case 'j':
        case '\n':
            ls->type_sel = (ls->type_sel + 1) % ACCOUNT_TYPE_COUNT;
            return true;
        default: {
            // Digits only, max 4
            int len = (int)strlen(ls->card_last4_buf);
            if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (ls->card_last4_pos > 0) {
                    memmove(&ls->card_last4_buf[ls->card_last4_pos - 1],
                            &ls->card_last4_buf[ls->card_last4_pos],
                            len - ls->card_last4_pos + 1);
                    ls->card_last4_pos--;
                }
            } else if (ch == KEY_LEFT) {
                if (ls->card_last4_pos > 0)
                    ls->card_last4_pos--;
            } else if (ch == KEY_RIGHT) {
                if (ls->card_last4_pos < len)
                    ls->card_last4_pos++;
            } else if (isdigit(ch) && len < 4) {
                memmove(&ls->card_last4_buf[ls->card_last4_pos + 1],
                        &ls->card_last4_buf[ls->card_last4_pos],
                        len - ls->card_last4_pos + 1);
                ls->card_last4_buf[ls->card_last4_pos] = (char)ch;
                ls->card_last4_pos++;
            }
            return true;
        }
        }
    }

    // Submit button is focused
    if (ls->cursor == -4) {
        switch (ch) {
        case KEY_UP:
        case 'k':
            ls->cursor = -2; // back to type selector
            return true;
        case KEY_DOWN:
        case 'j':
            if (ls->account_count > 0) {
                ls->cursor = 0;
                curs_set(0);
            }
            return true;
        case '\n':
            submit_account(ls);
            return true;
        }
    }

    // List is focused
    switch (ch) {
    case KEY_UP:
    case 'k':
        if (ls->cursor > 0) {
            ls->cursor--;
        } else {
            ls->cursor = -4; // go to [ Submit ] button
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
        return "q:Quit  \u2190\u2192:Change Type  \u2191:Name  \u2193:Next  "
               "\u2190:Sidebar";
    if (ls->cursor == -3)
        return "q:Quit  Enter:Add  \u2191:Type  \u2193:List  \u2190:Sidebar";
    return "q:Quit  \u2191\u2193:Navigate  \u2190:Sidebar";
}

void account_list_mark_dirty(account_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}

bool account_list_consume_added(account_list_state_t *ls) {
    if (!ls || !ls->account_added)
        return false;
    ls->account_added = false;
    return true;
}
