#include "ui/loan_list.h"

#include "db/query.h"
#include "models/account.h"
#include "ui/colors.h"
#include "ui/error_popup.h"
#include "ui/form.h"
#include "ui/resize.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    LOAN_FIELD_ACCOUNT = 0,
    LOAN_FIELD_KIND,
    LOAN_FIELD_START_DATE,
    LOAN_FIELD_RATE,
    LOAN_FIELD_INITIAL,
    LOAN_FIELD_PAYMENT,
    LOAN_FIELD_DAY,
    LOAN_FIELD_SPLIT_ESCROW,
    LOAN_FIELD_SUBMIT,
    LOAN_FIELD_COUNT,
} loan_field_t;

struct loan_list_state {
    sqlite3 *db;

    loan_profile_t *profiles;
    int profile_count;
    int profile_sel;

    txn_row_t *transactions;
    int txn_count;

    bool has_next_due;
    char next_due_date[11];
    bool has_remaining_principal;
    int64_t remaining_principal_cents;

    int cursor;
    int scroll_offset;
    char message[96];

    bool dirty;
    bool changed;
};

static int display_count(const loan_list_state_t *ls) {
    if (!ls || ls->profile_count <= 0)
        return 0;
    return 1 + ls->txn_count;
}

static const char *loan_kind_label(loan_kind_t kind) {
    return kind == LOAN_KIND_MORTGAGE ? "Mortgage" : "Car";
}

static void format_signed_cents(int64_t cents, bool show_plus, char *buf, int n) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    int64_t whole = abs_cents / 100;
    int64_t frac = abs_cents % 100;

    char raw[32];
    snprintf(raw, sizeof(raw), "%ld", (long)whole);
    int raw_len = (int)strlen(raw);

    char grouped[48];
    int gi = 0;
    for (int i = 0; i < raw_len; i++) {
        if (i > 0 && (raw_len - i) % 3 == 0)
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

static bool parse_cents_input(const char *buf, int64_t *out_cents) {
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

static bool parse_rate_bps_input(const char *buf, int *out_bps) {
    int64_t tmp = 0;
    if (!parse_cents_input(buf, &tmp))
        return false;
    if (tmp > INT_MAX)
        return false;
    *out_bps = (int)tmp;
    return true;
}

static bool parse_day_input(const char *buf, int *out_day) {
    if (!buf || !out_day)
        return false;
    while (isspace((unsigned char)*buf))
        buf++;
    if (*buf == '\0')
        return false;

    int day = 0;
    for (const char *p = buf; *p; p++) {
        if (isspace((unsigned char)*p))
            continue;
        if (!isdigit((unsigned char)*p))
            return false;
        day = day * 10 + (*p - '0');
    }
    if (day < 1 || day > 28)
        return false;
    *out_day = day;
    return true;
}

static bool validate_iso_date(const char *date) {
    if (!date || strlen(date) != 10)
        return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (date[i] != '-')
                return false;
        } else if (!isdigit((unsigned char)date[i])) {
            return false;
        }
    }
    return true;
}

static void handle_text_input(char *buf, int *pos, int maxlen, int ch,
                              bool allow_dot) {
    int len = (int)strlen(buf);

    if (ch == KEY_LEFT) {
        if (*pos > 0)
            (*pos)--;
    } else if (ch == KEY_RIGHT) {
        if (*pos < len)
            (*pos)++;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (*pos > 0) {
            memmove(&buf[*pos - 1], &buf[*pos], (size_t)(len - *pos + 1));
            (*pos)--;
        }
    } else if (isprint(ch) && len < maxlen - 1) {
        if (!allow_dot && ch == '.')
            return;
        memmove(&buf[*pos + 1], &buf[*pos], (size_t)(len - *pos + 1));
        buf[*pos] = (char)ch;
        (*pos)++;
    }
}

static bool has_profile_for_account(const loan_list_state_t *ls, int64_t account_id,
                                    int64_t except_account_id) {
    if (!ls || account_id <= 0)
        return false;
    for (int i = 0; i < ls->profile_count; i++) {
        if (ls->profiles[i].account_id == account_id && account_id != except_account_id)
            return true;
    }
    return false;
}

static bool confirm_simple(WINDOW *parent, const char *title, const char *line1,
                           const char *line2, const char *hint) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = 8;
    int win_w = 70;
    if (win_h > ph)
        win_h = ph;
    if (win_w > pw)
        win_w = pw;
    if (win_h < 5 || win_w < 30)
        return false;

    int py, px;
    getbegyx(parent, py, px);
    WINDOW *w = newwin(win_h, win_w, py + (ph - win_h) / 2, px + (pw - win_w) / 2);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);
    mvwprintw(w, 0, 2, "%s", title ? title : " Confirm ");
    mvwprintw(w, 2, 2, "%-*.*s", win_w - 4, win_w - 4, line1 ? line1 : "");
    mvwprintw(w, 3, 2, "%-*.*s", win_w - 4, win_w - 4, line2 ? line2 : "");
    mvwprintw(w, win_h - 2, 2, "%s", hint ? hint : "y:Confirm  n:Cancel");
    wrefresh(w);

    bool confirm = false;
    bool done = false;
    while (!done) {
        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            done = true;
            confirm = false;
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
        default:
            break;
        }
    }

    delwin(w);
    touchwin(parent);
    return confirm;
}

static bool show_profile_form(loan_list_state_t *ls, WINDOW *parent,
                              const loan_profile_t *existing) {
    if (!ls || !parent)
        return false;

    account_t *accounts = NULL;
    int account_count = db_get_accounts(ls->db, &accounts);
    if (account_count <= 0) {
        free(accounts);
        ui_show_error_popup(parent, " Loans ", "No accounts available");
        return false;
    }

    bool is_edit = (existing != NULL);
    int available_count = 0;
    for (int i = 0; i < account_count; i++) {
        if (!has_profile_for_account(ls, accounts[i].id, is_edit ? existing->account_id : 0))
            available_count++;
    }
    if (!is_edit && available_count == 0) {
        free(accounts);
        ui_show_error_popup(parent, " Loans ",
                            "All accounts already have loan profiles");
        return false;
    }

    int ph, pw;
    getmaxyx(parent, ph, pw);
    int win_h = 18;
    int win_w = 68;
    if (win_h > ph)
        win_h = ph;
    if (win_w > pw)
        win_w = pw;
    win_h = 21;
    if (win_h > ph)
        win_h = ph;
    if (win_h < 14 || win_w < 48) {
        free(accounts);
        return false;
    }

    int py, px;
    getbegyx(parent, py, px);
    WINDOW *w = newwin(win_h, win_w, py + (ph - win_h) / 2, px + (pw - win_w) / 2);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));

    int account_idx = 0;
    if (is_edit) {
        for (int i = 0; i < account_count; i++) {
            if (accounts[i].id == existing->account_id) {
                account_idx = i;
                break;
            }
        }
    } else {
        while (account_idx < account_count &&
               has_profile_for_account(ls, accounts[account_idx].id, 0)) {
            account_idx++;
        }
        if (account_idx >= account_count)
            account_idx = 0;
    }

    loan_kind_t kind = is_edit ? existing->loan_kind : LOAN_KIND_CAR;
    char start_date[16] = "";
    char rate[24] = "";
    char initial[24] = "";
    char payment[24] = "";
    char day[8] = "1";
    char split_escrow[24] = "0.00";
    if (is_edit) {
        snprintf(start_date, sizeof(start_date), "%s", existing->start_date);
        snprintf(rate, sizeof(rate), "%d.%02d", existing->interest_rate_bps / 100,
                 existing->interest_rate_bps % 100);
        snprintf(initial, sizeof(initial), "%ld.%02ld",
                 (long)(existing->initial_principal_cents / 100),
                 (long)(existing->initial_principal_cents % 100));
        snprintf(payment, sizeof(payment), "%ld.%02ld",
                 (long)(existing->scheduled_payment_cents / 100),
                 (long)(existing->scheduled_payment_cents % 100));
        snprintf(day, sizeof(day), "%d", existing->payment_day);
        snprintf(split_escrow, sizeof(split_escrow), "%ld.%02ld",
                 (long)(existing->split_escrow_cents / 100),
                 (long)(existing->split_escrow_cents % 100));
    }

    int start_pos = (int)strlen(start_date);
    int rate_pos = (int)strlen(rate);
    int initial_pos = (int)strlen(initial);
    int payment_pos = (int)strlen(payment);
    int day_pos = (int)strlen(day);
    int split_escrow_pos = (int)strlen(split_escrow);

    loan_field_t field = is_edit ? LOAN_FIELD_KIND : LOAN_FIELD_ACCOUNT;
    char error[96] = "";

    bool saved = false;
    bool done = false;
    while (!done) {
        werase(w);
        box(w, 0, 0);
        mvwprintw(w, 0, 2, "%s", is_edit ? " Edit Loan Profile " : " Add Loan Profile ");

        int label_col = 2;
        int field_col = 20;
        int row = 2;

        mvwprintw(w, row, label_col, "Account:");
        if (!is_edit && field == LOAN_FIELD_ACCOUNT)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        if (is_edit)
            mvwprintw(w, row, field_col, "%-.40s", accounts[account_idx].name);
        else
            mvwprintw(w, row, field_col, "< %-36.36s >", accounts[account_idx].name);
        if (!is_edit && field == LOAN_FIELD_ACCOUNT)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 2;

        mvwprintw(w, row, label_col, "Loan kind:");
        if (field == LOAN_FIELD_KIND)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        mvwprintw(w, row, field_col, "< %-12s >", loan_kind_label(kind));
        if (field == LOAN_FIELD_KIND)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 2;

        mvwprintw(w, row, label_col, "Start date:");
        if (field == LOAN_FIELD_START_DATE)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        mvwprintw(w, row, field_col, "%-12s", start_date);
        if (field == LOAN_FIELD_START_DATE)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 1;

        mvwprintw(w, row, label_col, "Rate (%%):");
        if (field == LOAN_FIELD_RATE)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        mvwprintw(w, row, field_col, "%-12s", rate);
        if (field == LOAN_FIELD_RATE)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 1;

        mvwprintw(w, row, label_col, "Initial amount:");
        if (field == LOAN_FIELD_INITIAL)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        mvwprintw(w, row, field_col, "%-12s", initial);
        if (field == LOAN_FIELD_INITIAL)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 1;

        mvwprintw(w, row, label_col, "Scheduled pay:");
        if (field == LOAN_FIELD_PAYMENT)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        mvwprintw(w, row, field_col, "%-12s", payment);
        if (field == LOAN_FIELD_PAYMENT)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 1;

        mvwprintw(w, row, label_col, "Payment day:");
        if (field == LOAN_FIELD_DAY)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        mvwprintw(w, row, field_col, "%-4s", day);
        if (field == LOAN_FIELD_DAY)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 1;

        mvwprintw(w, row, label_col, "Escrow split:");
        if (field == LOAN_FIELD_SPLIT_ESCROW)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        mvwprintw(w, row, field_col, "%-12s", split_escrow);
        if (field == LOAN_FIELD_SPLIT_ESCROW)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        row += 2;

        if (field == LOAN_FIELD_SUBMIT)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        mvwprintw(w, row, (win_w - 10) / 2, "[ Submit ]");
        if (field == LOAN_FIELD_SUBMIT)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

        if (error[0] != '\0') {
            wattron(w, A_BOLD);
            mvwprintw(w, win_h - 2, 2, "%s", error);
            wattroff(w, A_BOLD);
        } else {
            mvwprintw(w, win_h - 2, 2, "Ctrl+S:Save  Esc:Cancel");
        }

        if (field == LOAN_FIELD_SUBMIT || field == LOAN_FIELD_ACCOUNT ||
            field == LOAN_FIELD_KIND) {
            curs_set(0);
        } else {
            curs_set(1);
            int cursor_x = field_col;
            int cursor_y = 0;
            switch (field) {
            case LOAN_FIELD_START_DATE:
                cursor_y = 6;
                cursor_x += start_pos;
                break;
            case LOAN_FIELD_RATE:
                cursor_y = 7;
                cursor_x += rate_pos;
                break;
            case LOAN_FIELD_INITIAL:
                cursor_y = 8;
                cursor_x += initial_pos;
                break;
            case LOAN_FIELD_PAYMENT:
                cursor_y = 9;
                cursor_x += payment_pos;
                break;
            case LOAN_FIELD_DAY:
                cursor_y = 10;
                cursor_x += day_pos;
                break;
            case LOAN_FIELD_SPLIT_ESCROW:
                cursor_y = 11;
                cursor_x += split_escrow_pos;
                break;
            default:
                cursor_y = 6;
                break;
            }
            wmove(w, cursor_y, cursor_x);
        }

        wrefresh(w);

        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            done = true;
            continue;
        }
        error[0] = '\0';

        if (ch == 27) {
            done = true;
            continue;
        }

        if (ch == '\t' || ch == KEY_DOWN) {
            do {
                field = (loan_field_t)((field + 1) % LOAN_FIELD_COUNT);
            } while (is_edit && field == LOAN_FIELD_ACCOUNT);
            continue;
        }
        if (ch == KEY_BTAB || ch == KEY_UP) {
            do {
                field =
                    (loan_field_t)((field + LOAN_FIELD_COUNT - 1) % LOAN_FIELD_COUNT);
            } while (is_edit && field == LOAN_FIELD_ACCOUNT);
            continue;
        }

        if (!is_edit && field == LOAN_FIELD_ACCOUNT &&
            (ch == KEY_LEFT || ch == KEY_RIGHT)) {
            int step = (ch == KEY_RIGHT) ? 1 : -1;
            int idx = account_idx;
            do {
                idx += step;
                if (idx < 0)
                    idx = account_count - 1;
                if (idx >= account_count)
                    idx = 0;
            } while (has_profile_for_account(ls, accounts[idx].id, 0) && idx != account_idx);
            if (!has_profile_for_account(ls, accounts[idx].id, 0))
                account_idx = idx;
            continue;
        }

        if (field == LOAN_FIELD_KIND && (ch == KEY_LEFT || ch == KEY_RIGHT || ch == ' ')) {
            kind = (kind == LOAN_KIND_CAR) ? LOAN_KIND_MORTGAGE : LOAN_KIND_CAR;
            continue;
        }

        if (ch == 19 || (ch == '\n' && field == LOAN_FIELD_SUBMIT)) {
            int rate_bps = 0;
            int64_t initial_cents = 0;
            int64_t payment_cents = 0;
            int64_t split_escrow_cents = 0;
            int payment_day = 0;
            if (!validate_iso_date(start_date)) {
                snprintf(error, sizeof(error), "Start date must be YYYY-MM-DD");
                field = LOAN_FIELD_START_DATE;
                continue;
            }
            if (!parse_rate_bps_input(rate, &rate_bps)) {
                snprintf(error, sizeof(error), "Invalid interest rate");
                field = LOAN_FIELD_RATE;
                continue;
            }
            if (!parse_cents_input(initial, &initial_cents) || initial_cents <= 0) {
                snprintf(error, sizeof(error), "Initial amount must be > 0");
                field = LOAN_FIELD_INITIAL;
                continue;
            }
            if (!parse_cents_input(payment, &payment_cents) || payment_cents <= 0) {
                snprintf(error, sizeof(error), "Scheduled payment must be > 0");
                field = LOAN_FIELD_PAYMENT;
                continue;
            }
            if (!parse_day_input(day, &payment_day)) {
                snprintf(error, sizeof(error), "Payment day must be between 1 and 28");
                field = LOAN_FIELD_DAY;
                continue;
            }
            if (!parse_cents_input(split_escrow, &split_escrow_cents) ||
                split_escrow_cents < 0) {
                snprintf(error, sizeof(error), "Invalid escrow split amount");
                field = LOAN_FIELD_SPLIT_ESCROW;
                continue;
            }
            if (split_escrow_cents >= payment_cents) {
                snprintf(error, sizeof(error),
                         "Escrow must be less than scheduled payment");
                field = LOAN_FIELD_SPLIT_ESCROW;
                continue;
            }

            int64_t principal_cat = 0;
            int64_t interest_cat = 0;
            int64_t escrow_cat = 0;
            if (db_ensure_loan_split_categories(ls->db, kind, &principal_cat,
                                                &interest_cat, &escrow_cat) != 0) {
                snprintf(error, sizeof(error), "Failed to create split categories");
                continue;
            }

            loan_profile_t profile = {0};
            profile.account_id = accounts[account_idx].id;
            profile.loan_kind = kind;
            snprintf(profile.start_date, sizeof(profile.start_date), "%.10s",
                     start_date);
            profile.interest_rate_bps = rate_bps;
            profile.initial_principal_cents = initial_cents;
            profile.scheduled_payment_cents = payment_cents;
            profile.payment_day = payment_day;
            profile.split_principal_cents = 0;
            profile.split_interest_cents = 0;
            profile.split_escrow_cents = split_escrow_cents;
            profile.split_principal_category_id = principal_cat;
            profile.split_interest_category_id = interest_cat;
            profile.split_escrow_category_id = split_escrow_cents > 0 ? escrow_cat : 0;

            int rc = db_upsert_loan_profile(ls->db, &profile);
            if (rc != 0) {
                snprintf(error, sizeof(error), "Failed to save loan profile");
                continue;
            }

            saved = true;
            done = true;
            continue;
        }

        switch (field) {
        case LOAN_FIELD_START_DATE:
            handle_text_input(start_date, &start_pos, (int)sizeof(start_date), ch,
                              false);
            break;
        case LOAN_FIELD_RATE:
            handle_text_input(rate, &rate_pos, (int)sizeof(rate), ch, true);
            break;
        case LOAN_FIELD_INITIAL:
            handle_text_input(initial, &initial_pos, (int)sizeof(initial), ch, true);
            break;
        case LOAN_FIELD_PAYMENT:
            handle_text_input(payment, &payment_pos, (int)sizeof(payment), ch, true);
            break;
        case LOAN_FIELD_DAY:
            handle_text_input(day, &day_pos, (int)sizeof(day), ch, false);
            break;
        case LOAN_FIELD_SPLIT_ESCROW:
            handle_text_input(split_escrow, &split_escrow_pos,
                              (int)sizeof(split_escrow), ch, true);
            break;
        default:
            break;
        }
    }

    curs_set(0);
    delwin(w);
    touchwin(parent);
    free(accounts);
    return saved;
}

static void reload(loan_list_state_t *ls) {
    if (!ls)
        return;

    int64_t selected_account_id = 0;
    if (ls->profile_count > 0 && ls->profile_sel >= 0 &&
        ls->profile_sel < ls->profile_count) {
        selected_account_id = ls->profiles[ls->profile_sel].account_id;
    }

    free(ls->profiles);
    ls->profiles = NULL;
    ls->profile_count = db_get_loan_profiles(ls->db, &ls->profiles);
    if (ls->profile_count < 0)
        ls->profile_count = 0;

    ls->profile_sel = 0;
    for (int i = 0; i < ls->profile_count; i++) {
        if (ls->profiles[i].account_id == selected_account_id) {
            ls->profile_sel = i;
            break;
        }
    }
    if (ls->profile_sel >= ls->profile_count)
        ls->profile_sel = ls->profile_count > 0 ? ls->profile_count - 1 : 0;

    free(ls->transactions);
    ls->transactions = NULL;
    ls->txn_count = 0;
    ls->has_next_due = false;
    ls->next_due_date[0] = '\0';
    ls->has_remaining_principal = false;
    ls->remaining_principal_cents = 0;

    if (ls->profile_count > 0) {
        int64_t account_id = ls->profiles[ls->profile_sel].account_id;
        ls->txn_count = db_get_transactions(ls->db, account_id, &ls->transactions);
        if (ls->txn_count < 0)
            ls->txn_count = 0;

        if (db_get_next_loan_payment_date(ls->db, account_id, ls->next_due_date) == 0)
            ls->has_next_due = true;

        if (db_get_loan_remaining_principal_cents(
                ls->db, account_id, &ls->remaining_principal_cents) == 0)
            ls->has_remaining_principal = true;
    }

    int dcount = display_count(ls);
    if (dcount <= 0) {
        if (ls->cursor < -1)
            ls->cursor = -1;
        if (ls->cursor > -1)
            ls->cursor = -1;
        ls->scroll_offset = 0;
    } else {
        if (ls->cursor < -1)
            ls->cursor = -1;
        if (ls->cursor >= dcount)
            ls->cursor = dcount - 1;
    }

    ls->dirty = false;
}

loan_list_state_t *loan_list_create(sqlite3 *db) {
    loan_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    ls->cursor = -1;
    ls->dirty = true;
    return ls;
}

void loan_list_destroy(loan_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->profiles);
    free(ls->transactions);
    free(ls);
}

void loan_list_draw(loan_list_state_t *ls, WINDOW *win, bool focused) {
    if (!ls || !win)
        return;
    if (ls->dirty)
        reload(ls);

    int h, w;
    getmaxyx(win, h, w);

    if (focused && ls->cursor == -1)
        wattron(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(win, 1, 2, "[ Add Loan Profile ]");
    if (focused && ls->cursor == -1)
        wattroff(win, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    if (ls->message[0] != '\0')
        mvwprintw(win, 2, 2, "%s", ls->message);

    if (ls->profile_count <= 0) {
        mvwprintw(win, 4, 2, "No loan profiles configured.");
        mvwprintw(win, 5, 2, "Press 'n' to add one for an existing account.");
        return;
    }

    const loan_profile_t *profile = &ls->profiles[ls->profile_sel];
    char principal[24];
    char scheduled[24];
    char split_escrow[24];
    char remaining_principal[24];
    format_signed_cents(profile->initial_principal_cents, false, principal,
                        sizeof(principal));
    format_signed_cents(profile->scheduled_payment_cents, false, scheduled,
                        sizeof(scheduled));
    format_signed_cents(profile->split_escrow_cents, false, split_escrow,
                        sizeof(split_escrow));
    format_signed_cents(ls->remaining_principal_cents, false, remaining_principal,
                        sizeof(remaining_principal));

    mvwprintw(win, 3, 2, "Loan %d/%d  Account: %s  Kind: %s", ls->profile_sel + 1,
              ls->profile_count, profile->account_name,
              loan_kind_label(profile->loan_kind));
    mvwprintw(win, 4, 2,
              "Start: %s  Rate: %d.%02d%%  Initial: %s  Scheduled: %s  Day:%d  Next:%s",
              profile->start_date, profile->interest_rate_bps / 100,
              profile->interest_rate_bps % 100, principal, scheduled,
              profile->payment_day, ls->has_next_due ? ls->next_due_date : "(n/a)");
    mvwprintw(win, 5, 2, "Escrow: %s  Principal/Interest: auto amortized",
              split_escrow);
    mvwprintw(win, 6, 2, "Remaining principal: %s",
              ls->has_remaining_principal ? remaining_principal : "(n/a)");

    int header_row = 8;
    int rule_row = 9;
    int data_start = 10;
    int visible_rows = h - data_start - 1;
    if (visible_rows < 1)
        visible_rows = 1;

    int date_w = 10;
    int amount_w = 12;
    int payee_w = 20;
    int desc_w = w - 4 - date_w - 1 - amount_w - 1 - payee_w - 1;
    if (desc_w < 8)
        desc_w = 8;

    int date_col = 2;
    int amount_col = date_col + date_w + 1;
    int payee_col = amount_col + amount_w + 1;
    int desc_col = payee_col + payee_w + 1;

    wattron(win, A_BOLD);
    mvwprintw(win, header_row, date_col, "%-*s", date_w, "Date");
    mvwprintw(win, header_row, amount_col, "%*s", amount_w, "Amount");
    mvwprintw(win, header_row, payee_col, "%-*s", payee_w, "Payee");
    mvwprintw(win, header_row, desc_col, "%-*s", desc_w, "Description");
    wattroff(win, A_BOLD);
    mvwhline(win, rule_row, 2, ACS_HLINE, w - 4);

    int dcount = display_count(ls);
    if (dcount <= 0) {
        mvwprintw(win, data_start, 2, "No rows");
        return;
    }

    if (ls->cursor < 0) {
        ls->scroll_offset = 0;
    } else if (ls->cursor < ls->scroll_offset)
        ls->scroll_offset = ls->cursor;
    if (ls->cursor >= 0 && ls->cursor >= ls->scroll_offset + visible_rows)
        ls->scroll_offset = ls->cursor - visible_rows + 1;
    if (ls->scroll_offset < 0)
        ls->scroll_offset = 0;
    int max_scroll = dcount - visible_rows;
    if (max_scroll < 0)
        max_scroll = 0;
    if (ls->scroll_offset > max_scroll)
        ls->scroll_offset = max_scroll;

    for (int i = 0; i < visible_rows; i++) {
        int idx = ls->scroll_offset + i;
        if (idx >= dcount)
            break;
        int row = data_start + i;
        bool is_phantom = (idx == 0);
        bool selected = (idx == ls->cursor && focused);
        if (is_phantom) {
            if (selected)
                wattron(win, COLOR_PAIR(COLOR_STATUS));
            else
                wattron(win, COLOR_PAIR(COLOR_STATUS) | A_DIM);
        } else if (selected) {
            wattron(win, COLOR_PAIR(COLOR_SELECTED));
        }

        mvwprintw(win, row, 2, "%*s", w - 4, "");
        if (is_phantom) {
            char amt[24];
            format_signed_cents(-profile->scheduled_payment_cents, false, amt,
                                sizeof(amt));
            mvwprintw(win, row, date_col, "%-*s", date_w,
                      ls->has_next_due ? ls->next_due_date : "(n/a)");
            mvwprintw(win, row, amount_col, "%*.*s", amount_w, amount_w, amt);
            mvwprintw(win, row, payee_col, "%-*.*s", payee_w, payee_w,
                      "[Next Payment]");
            mvwprintw(win, row, desc_col, "%-*.*s", desc_w, desc_w,
                      "Press Enter to enact payment");
        } else {
            const txn_row_t *t = &ls->transactions[idx - 1];
            char amt[24];
            int64_t signed_cents = (t->type == TRANSACTION_EXPENSE)
                                       ? -t->amount_cents
                                       : t->amount_cents;
            format_signed_cents(signed_cents, true, amt, sizeof(amt));

            mvwprintw(win, row, date_col, "%-*.*s", date_w, date_w, t->effective_date);
            if (!selected) {
                if (t->type == TRANSACTION_EXPENSE)
                    wattron(win, COLOR_PAIR(COLOR_EXPENSE));
                else if (t->type == TRANSACTION_INCOME)
                    wattron(win, COLOR_PAIR(COLOR_INCOME));
            }
            mvwprintw(win, row, amount_col, "%*.*s", amount_w, amount_w, amt);
            if (!selected) {
                if (t->type == TRANSACTION_EXPENSE)
                    wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
                else if (t->type == TRANSACTION_INCOME)
                    wattroff(win, COLOR_PAIR(COLOR_INCOME));
            }
            mvwprintw(win, row, payee_col, "%-*.*s", payee_w, payee_w, t->payee);
            mvwprintw(win, row, desc_col, "%-*.*s", desc_w, desc_w, t->description);
        }

        if (is_phantom) {
            if (selected)
                wattroff(win, COLOR_PAIR(COLOR_STATUS));
            else
                wattroff(win, COLOR_PAIR(COLOR_STATUS) | A_DIM);
        } else if (selected) {
            wattroff(win, COLOR_PAIR(COLOR_SELECTED));
        }
    }
}

bool loan_list_handle_input(loan_list_state_t *ls, WINDOW *parent, int ch) {
    if (!ls || !parent)
        return false;

    if (ls->dirty)
        reload(ls);

    switch (ch) {
    case 'a':
    case 'n':
        if (show_profile_form(ls, parent, NULL)) {
            ls->dirty = true;
            ls->changed = true;
            snprintf(ls->message, sizeof(ls->message), "Loan profile saved");
        }
        return true;
    case 'e':
        if (ls->profile_count <= 0)
            return true;
        if (ls->cursor <= 0 || ls->cursor > ls->txn_count) {
            ui_show_error_popup(parent, " Loans ",
                                "Select an existing transaction row to edit");
            return true;
        }
        {
            const txn_row_t *row = &ls->transactions[ls->cursor - 1];
            transaction_t txn = {0};
            int rc = db_get_transaction_by_id(ls->db, (int)row->id, &txn);
            if (rc == 0) {
                form_result_t res = form_transaction(parent, ls->db, &txn, true);
                if (res == FORM_SAVED) {
                    ls->dirty = true;
                    ls->changed = true;
                    snprintf(ls->message, sizeof(ls->message),
                             "Transaction updated");
                }
            } else {
                ui_show_error_popup(parent, " Loans ",
                                    "Failed to load selected transaction");
                ls->dirty = true;
            }
        }
        return true;
    case 'E':
        if (ls->profile_count <= 0)
            return true;
        if (show_profile_form(ls, parent, &ls->profiles[ls->profile_sel])) {
            ls->dirty = true;
            ls->changed = true;
            snprintf(ls->message, sizeof(ls->message), "Loan profile updated");
        }
        return true;
    case 'd':
        if (ls->profile_count <= 0)
            return true;
        if (ls->cursor <= 0 || ls->cursor > ls->txn_count) {
            ui_show_error_popup(parent, " Loans ",
                                "Select an existing transaction row to delete");
            return true;
        }
        {
            const txn_row_t *row = &ls->transactions[ls->cursor - 1];
            char line1[96];
            char line2[96];
            snprintf(line1, sizeof(line1), "Delete selected transaction?");
            snprintf(line2, sizeof(line2), "%.10s  %.72s", row->effective_date,
                     row->payee[0] ? row->payee : "(no payee)");
            if (!confirm_simple(parent, " Delete Transaction ", line1, line2,
                                "y:Delete  n:Cancel")) {
                return true;
            }
            int rc = db_delete_transaction(ls->db, (int)row->id);
            if (rc == 0) {
                ls->dirty = true;
                ls->changed = true;
                if (ls->cursor > 0)
                    ls->cursor--;
                snprintf(ls->message, sizeof(ls->message),
                         "Transaction deleted");
            } else {
                ui_show_error_popup(parent, " Loans ",
                                    "Failed to delete selected transaction");
            }
        }
        return true;
    case 'D':
        if (ls->profile_count <= 0)
            return true;
        {
            const loan_profile_t *p = &ls->profiles[ls->profile_sel];
            char line1[96];
            char line2[96];
            snprintf(line1, sizeof(line1), "Delete loan profile for '%s'?", p->account_name);
            snprintf(line2, sizeof(line2), "Transactions in the account are not deleted.");
            if (!confirm_simple(parent, " Delete Loan Profile ", line1, line2,
                                "y:Delete  n:Cancel")) {
                return true;
            }
            int rc = db_delete_loan_profile(ls->db, p->account_id);
            if (rc == 0) {
                ls->dirty = true;
                ls->changed = true;
                snprintf(ls->message, sizeof(ls->message), "Loan profile deleted");
            } else {
                ui_show_error_popup(parent, " Loans ",
                                    "Failed to delete loan profile");
            }
        }
        return true;
    case KEY_UP:
    case 'k':
        if (ls->cursor > -1)
            ls->cursor--;
        return true;
    case KEY_DOWN:
    case 'j':
        if (display_count(ls) > 0 && ls->cursor < display_count(ls) - 1)
            ls->cursor++;
        return true;
    case 'g':
    case KEY_HOME:
        ls->cursor = -1;
        return true;
    case 'G':
    case KEY_END:
        if (display_count(ls) > 0)
            ls->cursor = display_count(ls) - 1;
        return true;
    case '\n':
        if (ls->cursor == -1)
            return loan_list_handle_input(ls, parent, 'n');
        if (ls->profile_count <= 0 || ls->cursor != 0)
            return true;
        {
            const loan_profile_t *p = &ls->profiles[ls->profile_sel];
            char amt[24];
            format_signed_cents(p->scheduled_payment_cents, false, amt, sizeof(amt));
            char line1[96];
            char line2[96];
            snprintf(line1, sizeof(line1), "Post next payment (%s) on %s?", amt,
                     ls->has_next_due ? ls->next_due_date : "(n/a)");
            int64_t principal_cents = 0;
            int64_t interest_cents = 0;
            int64_t escrow_cents = 0;
            if (db_get_next_loan_payment_breakdown(ls->db, p->account_id,
                                                   &principal_cents,
                                                   &interest_cents,
                                                   &escrow_cents) == 0) {
                char sp[24], si[24], se[24];
                format_signed_cents(principal_cents, false, sp, sizeof(sp));
                format_signed_cents(interest_cents, false, si, sizeof(si));
                format_signed_cents(escrow_cents, false, se, sizeof(se));
                snprintf(line2, sizeof(line2), "P:%s I:%s E:%s", sp, si, se);
            } else {
                snprintf(line2, sizeof(line2), "Using amortized principal/interest");
            }
            if (!confirm_simple(parent, " Enact Loan Payment ", line1, line2,
                                "y:Post payment  n:Cancel")) {
                return true;
            }
            int64_t txn_id = db_enact_loan_payment(ls->db, p->account_id);
            if (txn_id > 0) {
                ls->dirty = true;
                ls->changed = true;
                snprintf(ls->message, sizeof(ls->message), "Payment posted");
            } else {
                ui_show_error_popup(parent, " Loans ",
                                    "Failed to enact scheduled payment");
            }
        }
        return true;
    default:
        if (ch >= '1' && ch <= '9') {
            int idx = ch - '1';
            if (idx < ls->profile_count && idx != ls->profile_sel) {
                ls->profile_sel = idx;
                ls->cursor = 0;
                ls->scroll_offset = 0;
                ls->dirty = true;
            }
            return true;
        }
        return false;
    }
}

const char *loan_list_status_hint(const loan_list_state_t *ls) {
    if (!ls)
        return "";
    if (ls->cursor == -1)
        return "Enter add-profile  j/k move  n add  E edit-loan  D del-loan  <- back";
    if (ls->profile_count <= 0)
        return "Enter add-profile  n add  <- back";
    if (ls->cursor == 0)
        return "1-9 loan  j/k move  Enter enact-payment  n add  E edit-loan  D del-loan  <- back";
    return "1-9 loan  j/k move  e edit-txn  d del-txn  E edit-loan  D del-loan  <- back";
}

void loan_list_mark_dirty(loan_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}

bool loan_list_consume_changed(loan_list_state_t *ls) {
    if (!ls)
        return false;
    bool changed = ls->changed;
    ls->changed = false;
    return changed;
}

void loan_list_focus_add_button(loan_list_state_t *ls) {
    if (!ls)
        return;
    ls->cursor = -1;
    ls->scroll_offset = 0;
}
