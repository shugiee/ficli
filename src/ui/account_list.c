#include "ui/account_list.h"
#include "db/query.h"
#include "models/account.h"
#include "ui/colors.h"
#include "ui/form.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Display labels for account types (indexes match account_type_t enum)
static const char *account_type_labels[] = {"Cash",           "Checking",
                                            "Savings",        "Credit Card",
                                            "Physical Asset", "Investment"};

#define CURSOR_NAME -1
#define CURSOR_TYPE -2
#define CURSOR_CARD -3
#define CURSOR_SUBMIT -4
#define CURSOR_ADD_BUTTON -5

struct account_list_state {
    sqlite3 *db;
    account_t *accounts;
    int account_count;
    int cursor; // negative = controls, 0+ = list items
    int scroll_offset;
    bool show_add_form;
    char name_buf[64];
    int name_pos;
    int type_sel;           // index into account_type_labels
    char card_last4_buf[5]; // up to 4 digits
    int card_last4_pos;
    char message[64];
    bool dirty;
    bool changed;
};

static bool confirm_delete_account(WINDOW *parent, const char *account_name,
                                   int txn_count) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = 8;
    int win_w = 58;
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

    char line1[96];
    char line2[96];
    snprintf(line1, sizeof(line1), "Delete account '%-.32s'?", account_name);
    if (txn_count > 0) {
        snprintf(line2, sizeof(line2), "Also delete %d related transaction%s?",
                 txn_count, txn_count == 1 ? "" : "s");
    } else {
        snprintf(line2, sizeof(line2), "This account has no transactions.");
    }

    mvwprintw(w, 1, 2, "%s", line1);
    mvwprintw(w, 3, 2, "%s", line2);
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

static void reload(account_list_state_t *ls) {
    free(ls->accounts);
    ls->accounts = NULL;
    ls->account_count = 0;

    ls->account_count = db_get_accounts(ls->db, &ls->accounts);
    if (ls->account_count < 0)
        ls->account_count = 0;

    if (ls->cursor >= 0 && ls->cursor >= ls->account_count)
        ls->cursor =
            ls->account_count > 0 ? ls->account_count - 1 : CURSOR_ADD_BUTTON;

    ls->dirty = false;
}

account_list_state_t *account_list_create(sqlite3 *db) {
    account_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    ls->cursor = CURSOR_ADD_BUTTON;
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

    bool add_button_active = (ls->cursor == CURSOR_ADD_BUTTON && focused);
    if (add_button_active)
        wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(win, 1, 2, "[ Add Account ]");
    if (add_button_active)
        wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

    int message_row = 2;
    int header_row = 3;
    int rule_row = 4;
    int data_row_start = 5;

    if (ls->show_add_form) {
        wattron(win, A_BOLD);
        mvwprintw(win, 2, 2, "New Account");
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
        mvwprintw(win, 4, 8, "< %-16s >", account_type_labels[ls->type_sel]);
        if (type_active)
            wattroff(win, COLOR_PAIR(COLOR_SELECTED));

        bool card_active = (ls->cursor == CURSOR_CARD && focused);
        if (ls->type_sel == ACCOUNT_CREDIT_CARD) {
            mvwprintw(win, 5, 2, "Card last 4: ");
            if (card_active)
                wattron(win, COLOR_PAIR(COLOR_SELECTED));
            mvwprintw(win, 5, 15, "%-4s", ls->card_last4_buf);
            if (card_active) {
                wattroff(win, COLOR_PAIR(COLOR_SELECTED));
                curs_set(1);
                wmove(win, 5, 15 + ls->card_last4_pos);
            }
        } else {
            mvwprintw(win, 5, 2, "%-*s", field_w + 10, "");
        }

        bool submit_button_focused = (ls->cursor == CURSOR_SUBMIT && focused);
        if (submit_button_focused)
            wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        mvwprintw(win, 6, 2, "[ Submit ]");
        if (submit_button_focused)
            wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

        message_row = 7;
        header_row = 8;
        rule_row = 9;
        data_row_start = 10;
    } else {
        curs_set(0);
    }

    if (ls->message[0] != '\0') {
        mvwprintw(win, message_row, 2, "%s", ls->message);
    } else {
        mvwprintw(win, message_row, 2, "%-*s", field_w + 10, "");
    }

    wattron(win, A_BOLD);
    mvwprintw(win, header_row, 2, "Accounts");
    wattroff(win, A_BOLD);

    wmove(win, rule_row, 2);
    for (int i = 2; i < w - 2; i++)
        waddch(win, ACS_HLINE);

    // -- Account list --
    int visible_rows = h - 1 - data_row_start;
    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->account_count == 0) {
        const char *msg = "No accounts";
        int mlen = (int)strlen(msg);
        int row = data_row_start + visible_rows / 2;
        if (row >= h - 1)
            row = data_row_start;
        mvwprintw(win, row, (w - mlen) / 2, "%s", msg);
        return;
    }

    // Clamp cursor/scroll for list
    if (ls->cursor >= ls->account_count)
        ls->cursor =
            ls->account_count > 0 ? ls->account_count - 1 : CURSOR_ADD_BUTTON;

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

        int row = data_row_start + i;
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

static bool submit_account(account_list_state_t *ls) {
    if (ls->name_buf[0] == '\0') {
        snprintf(ls->message, sizeof(ls->message), "Name cannot be empty");
        return false;
    }
    const char *card =
        (ls->type_sel == ACCOUNT_CREDIT_CARD) ? ls->card_last4_buf : NULL;
    int64_t id = db_insert_account(ls->db, ls->name_buf,
                                   (account_type_t)ls->type_sel, card);
    if (id < 0) {
        snprintf(ls->message, sizeof(ls->message), "Error adding account");
        return false;
    } else {
        snprintf(ls->message, sizeof(ls->message), "Added: %.56s",
                 ls->name_buf);
        ls->name_buf[0] = '\0';
        ls->name_pos = 0;
        ls->type_sel = 0;
        ls->card_last4_buf[0] = '\0';
        ls->card_last4_pos = 0;
        ls->dirty = true;
        ls->changed = true;
        return true;
    }
}

bool account_list_handle_input(account_list_state_t *ls, WINDOW *parent, int ch) {
    ls->message[0] = '\0';

    if (!ls->show_add_form && ls->cursor == CURSOR_ADD_BUTTON) {
        switch (ch) {
        case '\n':
            ls->show_add_form = true;
            ls->cursor = CURSOR_NAME;
            return true;
        case KEY_DOWN:
        case 'j':
            if (ls->account_count > 0)
                ls->cursor = 0;
            return true;
        default:
            return false;
        }
    }

    // Name input field is focused
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
            ls->cursor = CURSOR_TYPE; // move to type selector
            return true;
        }
        // Text input
        handle_text_input(ls->name_buf, &ls->name_pos,
                          (int)sizeof(ls->name_buf), ch);
        return true;
    }

    // Type selector is focused
    if (ls->show_add_form && ls->cursor == CURSOR_TYPE) {
        switch (ch) {
        case 27:
            ls->show_add_form = false;
            ls->cursor = CURSOR_ADD_BUTTON;
            return true;
        case KEY_UP:
        case 'k':
            ls->cursor = CURSOR_NAME; // back to name input
            return true;
        case KEY_DOWN:
        case 'j':
        case '\n':
            if (ls->type_sel == ACCOUNT_CREDIT_CARD) {
                ls->cursor = CURSOR_CARD; // go to card last 4
            } else {
                // Go to submit button
                ls->cursor = CURSOR_SUBMIT;
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
    if (ls->show_add_form && ls->cursor == CURSOR_CARD) {
        switch (ch) {
        case 27:
            ls->show_add_form = false;
            ls->cursor = CURSOR_ADD_BUTTON;
            return true;
        case KEY_UP:
        case 'k':
            ls->cursor = CURSOR_TYPE; // back to type selector
            return true;
        case KEY_DOWN:
        case 'j':
        case '\n':
            ls->cursor = CURSOR_SUBMIT;
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
    if (ls->show_add_form && ls->cursor == CURSOR_SUBMIT) {
        switch (ch) {
        case 27:
            ls->show_add_form = false;
            ls->cursor = CURSOR_ADD_BUTTON;
            return true;
        case KEY_UP:
        case 'k':
            ls->cursor =
                (ls->type_sel == ACCOUNT_CREDIT_CARD) ? CURSOR_CARD : CURSOR_TYPE;
            return true;
        case KEY_DOWN:
        case 'j':
            if (ls->account_count > 0) {
                ls->cursor = 0;
                curs_set(0);
            }
            return true;
        case '\n':
            if (submit_account(ls)) {
                ls->show_add_form = false;
                ls->cursor = CURSOR_ADD_BUTTON;
            }
            return true;
        }
    }

    // List is focused
    switch (ch) {
    case 'e':
        if (ls->cursor >= 0 && ls->cursor < ls->account_count) {
            account_t account = ls->accounts[ls->cursor];
            form_result_t res = form_account(parent, ls->db, &account, true);
            if (res == FORM_SAVED) {
                ls->dirty = true;
                ls->changed = true;
                snprintf(ls->message, sizeof(ls->message), "Updated: %.54s",
                         account.name);
            }
        }
        return true;
    case 'd':
        if (ls->cursor >= 0 && ls->cursor < ls->account_count) {
            account_t account = ls->accounts[ls->cursor];
            int txn_count = db_count_transactions_for_account(ls->db, account.id);
            if (txn_count < 0) {
                snprintf(ls->message, sizeof(ls->message), "Error checking account");
                return true;
            }

            if (!confirm_delete_account(parent, account.name, txn_count)) {
                snprintf(ls->message, sizeof(ls->message), "Delete cancelled");
                return true;
            }

            bool delete_txns = (txn_count > 0);
            int rc = db_delete_account(ls->db, account.id, delete_txns);
            if (rc == 0) {
                ls->dirty = true;
                ls->changed = true;
                snprintf(ls->message, sizeof(ls->message), "Deleted: %.54s",
                         account.name);
            } else if (rc == -3) {
                snprintf(ls->message, sizeof(ls->message),
                         "Account has related transactions");
            } else if (rc == -2) {
                snprintf(ls->message, sizeof(ls->message), "Account not found");
                ls->dirty = true;
                ls->changed = true;
            } else {
                snprintf(ls->message, sizeof(ls->message), "Error deleting account");
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
        if (ls->cursor < ls->account_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = CURSOR_ADD_BUTTON;
        return true;
    case KEY_END:
    case 'G':
        ls->cursor =
            ls->account_count > 0 ? ls->account_count - 1 : CURSOR_ADD_BUTTON;
        return true;
    default:
        return false;
    }
}

const char *account_list_status_hint(const account_list_state_t *ls) {
    if (ls->cursor == CURSOR_ADD_BUTTON)
        return "q:Quit  Enter:Show Add Form  \u2193:List  \u2190:Sidebar";
    if (ls->cursor == CURSOR_NAME)
        return "q:Quit  Enter:Next  Esc:Close Form  \u2193:Type  \u2190:Sidebar";
    if (ls->cursor == CURSOR_TYPE)
        return "q:Quit  \u2190\u2192:Change Type  \u2191:Name  \u2193:Next  Esc:Close Form";
    if (ls->cursor == CURSOR_CARD)
        return "q:Quit  Enter:Next  \u2191:Type  Esc:Close Form  \u2190:Sidebar";
    if (ls->cursor == CURSOR_SUBMIT)
        return "q:Quit  Enter:Submit  \u2191:Back  \u2193:List  Esc:Close Form";
    return "q:Quit  \u2191\u2193:Navigate  e:Edit  d:Delete  \u2190:Sidebar";
}

void account_list_mark_dirty(account_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}

bool account_list_consume_changed(account_list_state_t *ls) {
    if (!ls || !ls->changed)
        return false;
    ls->changed = false;
    return true;
}
