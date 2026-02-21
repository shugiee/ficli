#include "ui/account_list.h"
#include "db/query.h"
#include "models/account.h"
#include "ui/colors.h"
#include "ui/error_popup.h"
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

    if (!ls->show_add_form) {
        bool add_button_active = (ls->cursor == CURSOR_ADD_BUTTON && focused);
        if (add_button_active)
            wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        mvwprintw(win, 1, 2, "[ Add Account ]");
        if (add_button_active)
            wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    }

    int message_row = 2;
    int header_row = 3;
    int rule_row = 4;
    int data_row_start = 5;

    if (ls->show_add_form) {
        bool show_card_field = (ls->type_sel == ACCOUNT_CREDIT_CARD);
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
        int card_row = type_row + 1;
        int submit_row = show_card_field ? card_row + 2 : type_row + 2;
        int form_bottom = submit_row;

        draw_box(win, form_top, form_left_col, form_bottom, form_right_col);

        wattron(win, A_BOLD);
        mvwprintw(win, form_top, form_left_col + 2, " Add Account ");
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
        mvwprintw(win, type_row, form_field_col, "< %-16s >",
                  account_type_labels[ls->type_sel]);
        wattroff(win, type_field_color);

        bool card_active = (ls->cursor == CURSOR_CARD && focused);
        if (show_card_field) {
            if (card_active)
                wattron(win, COLOR_PAIR(COLOR_INFO) | A_BOLD);
            mvwprintw(win, card_row, form_label_col, "Card last 4:");
            if (card_active)
                wattroff(win, COLOR_PAIR(COLOR_INFO) | A_BOLD);
            int card_field_color = COLOR_PAIR(card_active ? COLOR_FORM_ACTIVE
                                                          : COLOR_FORM_DROPDOWN);
            wattron(win, card_field_color);
            mvwprintw(win, card_row, form_field_col, "%-*s", form_field_w, "");
            mvwprintw(win, card_row, form_field_col, "%-4s", ls->card_last4_buf);
            wattroff(win, card_field_color);
            if (card_active) {
                curs_set(1);
                wmove(win, card_row, form_field_col + ls->card_last4_pos);
            }
        }

        bool submit_button_focused = (ls->cursor == CURSOR_SUBMIT && focused);
        const char *submit_label = "[ Submit ]";
        int submit_col = form_right_col - (int)strlen(submit_label) - 2;
        if (submit_col < form_left_col + 2)
            submit_col = form_left_col + 2;
        if (submit_button_focused)
            wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        mvwprintw(win, submit_row, submit_col, "%s", submit_label);
        if (submit_button_focused)
            wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

        message_row = form_bottom + 1;
        header_row = message_row + 1;
        rule_row = header_row + 1;
        data_row_start = rule_row + 1;
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

    int left_col = 2;
    int right_col = w - 2;
    int gap = 2;
    int max_name_len = 0;
    int max_type_len = 0;
    for (int i = 0; i < ls->account_count; i++) {
        int name_len = (int)strlen(ls->accounts[i].name);
        if (name_len > max_name_len)
            max_name_len = name_len;

        const char *tlabel = account_type_labels[ls->accounts[i].type];
        char type_tag[32];
        if (ls->accounts[i].type == ACCOUNT_CREDIT_CARD &&
            ls->accounts[i].card_last4[0] != '\0') {
            snprintf(type_tag, sizeof(type_tag), "[%s ****%s]", tlabel,
                     ls->accounts[i].card_last4);
        } else {
            snprintf(type_tag, sizeof(type_tag), "[%s]", tlabel);
        }
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
        if (idx >= ls->account_count)
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
                  ls->accounts[idx].name);

        const char *tlabel = account_type_labels[ls->accounts[idx].type];
        char type_tag[32];
        if (ls->accounts[idx].type == ACCOUNT_CREDIT_CARD &&
            ls->accounts[idx].card_last4[0] != '\0') {
            snprintf(type_tag, sizeof(type_tag), "[%s ****%s]", tlabel,
                     ls->accounts[idx].card_last4);
        } else {
            snprintf(type_tag, sizeof(type_tag), "[%s]", tlabel);
        }
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

static bool submit_account(account_list_state_t *ls, WINDOW *parent) {
    if (ls->name_buf[0] == '\0') {
        snprintf(ls->message, sizeof(ls->message), "Name cannot be empty");
        return false;
    }
    const char *card =
        (ls->type_sel == ACCOUNT_CREDIT_CARD) ? ls->card_last4_buf : NULL;
    int64_t id = db_insert_account(ls->db, ls->name_buf,
                                   (account_type_t)ls->type_sel, card);
    if (id == -2) {
        ui_show_error_popup(parent, " Account Error ",
                            "An account with that name already exists.");
        return false;
    }
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

    if (ls->show_add_form && ls->cursor == CURSOR_ADD_BUTTON) {
        switch (ch) {
        case 27:
            ls->show_add_form = false;
            ls->cursor = CURSOR_ADD_BUTTON;
            return true;
        case KEY_UP:
        case 'k':
            if (ls->account_count > 0)
                ls->cursor = 0;
            return true;
        default:
            ls->cursor = CURSOR_NAME;
            return true;
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
            if (submit_account(ls, parent)) {
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
            ls->cursor = ls->show_add_form ? CURSOR_SUBMIT : CURSOR_ADD_BUTTON;
        }
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->account_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = ls->show_add_form ? CURSOR_NAME : CURSOR_ADD_BUTTON;
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
