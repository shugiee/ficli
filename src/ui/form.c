#include "ui/form.h"
#include "db/query.h"
#include "models/account.h"
#include "models/category.h"
#include "models/transaction.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FORM_WIDTH  50
#define FORM_HEIGHT 19
#define LABEL_COL   2
#define FIELD_COL   16
#define FIELD_WIDTH 30
#define MAX_DROP    5

enum {
    FIELD_TYPE,
    FIELD_AMOUNT,
    FIELD_ACCOUNT,
    FIELD_CATEGORY,
    FIELD_DATE,
    FIELD_DESC,
    FIELD_SUBMIT,
    FIELD_COUNT
};

enum {
    COLOR_FORM = 10,
    COLOR_FORM_ACTIVE
};

typedef struct {
    WINDOW *win;
    sqlite3 *db;

    int current_field;
    bool dropdown_open;
    int dropdown_sel;
    int dropdown_scroll;

    // Type toggle
    transaction_type_t txn_type;

    // Text fields
    char amount[32];
    int amount_pos;
    char date[11];
    int date_pos;
    char desc[256];
    int desc_pos;

    // Account dropdown
    account_t *accounts;
    int account_count;
    int account_sel;

    // Category dropdown
    category_t *categories;
    int category_count;
    int category_sel;

    // Error message
    char error[64];
} form_state_t;

static void form_load_categories(form_state_t *fs) {
    free(fs->categories);
    fs->categories = NULL;
    fs->category_count = 0;
    fs->category_sel = 0;

    category_type_t ctype = (fs->txn_type == TRANSACTION_INCOME)
        ? CATEGORY_INCOME : CATEGORY_EXPENSE;
    int count = db_get_categories(fs->db, ctype, &fs->categories);
    if (count > 0) {
        fs->category_count = count;
    }
}

static void form_init_state(form_state_t *fs, sqlite3 *db) {
    memset(fs, 0, sizeof(*fs));
    fs->db = db;
    fs->txn_type = TRANSACTION_EXPENSE;

    // Load accounts
    int count = db_get_accounts(db, &fs->accounts);
    if (count > 0) {
        fs->account_count = count;
    }

    // Load categories for current type
    form_load_categories(fs);

    // Default date to today
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char datebuf[32];
    snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    memcpy(fs->date, datebuf, 10);
    fs->date[10] = '\0';
    fs->date_pos = 10;
}

static void form_cleanup_state(form_state_t *fs) {
    free(fs->accounts);
    free(fs->categories);
    if (fs->win) {
        delwin(fs->win);
    }
}

static const char *field_labels[FIELD_SUBMIT] = {
    "Type",
    "Amount",
    "Account",
    "Category",
    "Date",
    "Description"
};

static int field_row(int field) {
    // Row within form window for each field (after title and border)
    return 2 + field * 2;
}

static void form_draw(form_state_t *fs) {
    WINDOW *w = fs->win;
    werase(w);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    // Title
    const char *title = " Add Transaction ";
    int tw = (int)strlen(title);
    int ww = getmaxx(w);
    mvwprintw(w, 0, (ww - tw) / 2, "%s", title);

    for (int i = 0; i < FIELD_SUBMIT; i++) {
        int row = field_row(i);
        bool active = (i == fs->current_field && !fs->dropdown_open);

        // Label
        mvwprintw(w, row, LABEL_COL, "%s:", field_labels[i]);

        // Field value
        if (active)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

        // Draw field background
        mvwprintw(w, row, FIELD_COL, "%-*s", FIELD_WIDTH, "");

        switch (i) {
        case FIELD_TYPE:
            if (fs->txn_type == TRANSACTION_EXPENSE)
                mvwprintw(w, row, FIELD_COL, "< Expense >");
            else
                mvwprintw(w, row, FIELD_COL, "< Income  >");
            break;
        case FIELD_AMOUNT:
            mvwprintw(w, row, FIELD_COL, "%s", fs->amount);
            break;
        case FIELD_ACCOUNT:
            if (fs->account_count > 0)
                mvwprintw(w, row, FIELD_COL, "%s [v]",
                          fs->accounts[fs->account_sel].name);
            else
                mvwprintw(w, row, FIELD_COL, "(none)");
            break;
        case FIELD_CATEGORY:
            if (fs->category_count > 0)
                mvwprintw(w, row, FIELD_COL, "%s [v]",
                          fs->categories[fs->category_sel].name);
            else
                mvwprintw(w, row, FIELD_COL, "(none)");
            break;
        case FIELD_DATE:
            mvwprintw(w, row, FIELD_COL, "%s", fs->date);
            break;
        case FIELD_DESC:
            mvwprintw(w, row, FIELD_COL, "%s", fs->desc);
            break;
        }

        if (active)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    }

    // Submit button
    int submit_row = field_row(FIELD_SUBMIT);
    bool submit_active = (fs->current_field == FIELD_SUBMIT && !fs->dropdown_open);
    const char *btn = "[ Submit ]";
    int btn_len = (int)strlen(btn);
    if (submit_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(w, submit_row, (ww - btn_len) / 2, "%s", btn);
    if (submit_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

    // Error message
    if (fs->error[0] != '\0') {
        wattron(w, A_BOLD);
        mvwprintw(w, FORM_HEIGHT - 2, LABEL_COL, "%s", fs->error);
        wattroff(w, A_BOLD);
    }

    // Footer hints
    mvwprintw(w, FORM_HEIGHT - 1, 2, " C-s:Save  Esc:Cancel ");

    // Position cursor on active text field
    if (!fs->dropdown_open) {
        if (fs->current_field == FIELD_SUBMIT) {
            curs_set(0);
        } else {
            curs_set(1);
            int row = field_row(fs->current_field);
            switch (fs->current_field) {
            case FIELD_AMOUNT:
                wmove(w, row, FIELD_COL + fs->amount_pos);
                break;
            case FIELD_DATE:
                wmove(w, row, FIELD_COL + fs->date_pos);
                break;
            case FIELD_DESC:
                wmove(w, row, FIELD_COL + fs->desc_pos);
                break;
            default:
                wmove(w, row, FIELD_COL);
                break;
            }
        }
    }

    wrefresh(w);
}

static void form_draw_dropdown(form_state_t *fs) {
    WINDOW *w = fs->win;
    int base_row;
    int count;
    if (fs->current_field == FIELD_ACCOUNT) {
        base_row = field_row(FIELD_ACCOUNT) + 1;
        count = fs->account_count;
    } else {
        base_row = field_row(FIELD_CATEGORY) + 1;
        count = fs->category_count;
    }

    int visible = count < MAX_DROP ? count : MAX_DROP;

    // Clamp scroll
    if (fs->dropdown_sel < fs->dropdown_scroll)
        fs->dropdown_scroll = fs->dropdown_sel;
    if (fs->dropdown_sel >= fs->dropdown_scroll + visible)
        fs->dropdown_scroll = fs->dropdown_sel - visible + 1;

    for (int i = 0; i < visible; i++) {
        int idx = fs->dropdown_scroll + i;
        bool selected = (idx == fs->dropdown_sel);

        if (selected)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

        const char *name;
        if (fs->current_field == FIELD_ACCOUNT)
            name = fs->accounts[idx].name;
        else
            name = fs->categories[idx].name;

        mvwprintw(w, base_row + i, FIELD_COL, "%-*s", FIELD_WIDTH, name);

        if (selected)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    }

    // Show scroll indicators
    if (fs->dropdown_scroll > 0)
        mvwaddch(w, base_row, FIELD_COL + FIELD_WIDTH, ACS_UARROW);
    if (fs->dropdown_scroll + visible < count)
        mvwaddch(w, base_row + visible - 1, FIELD_COL + FIELD_WIDTH, ACS_DARROW);

    wmove(w, base_row + fs->dropdown_sel - fs->dropdown_scroll, FIELD_COL);
    wrefresh(w);
}

static void form_open_dropdown(form_state_t *fs) {
    int count = 0;
    int sel = 0;

    if (fs->current_field == FIELD_ACCOUNT) {
        count = fs->account_count;
        sel = fs->account_sel;
    } else if (fs->current_field == FIELD_CATEGORY) {
        count = fs->category_count;
        sel = fs->category_sel;
    }

    if (count == 0) return;

    fs->dropdown_open = true;
    fs->dropdown_sel = sel;
    fs->dropdown_scroll = 0;
    // Ensure selected item is visible
    int visible = count < MAX_DROP ? count : MAX_DROP;
    if (sel >= visible)
        fs->dropdown_scroll = sel - visible + 1;
}

static void form_close_dropdown(form_state_t *fs, bool accept) {
    if (accept) {
        if (fs->current_field == FIELD_ACCOUNT)
            fs->account_sel = fs->dropdown_sel;
        else if (fs->current_field == FIELD_CATEGORY)
            fs->category_sel = fs->dropdown_sel;
    }
    fs->dropdown_open = false;
    fs->dropdown_scroll = 0;
}

// Parse amount string to cents. Returns -1 on invalid input.
static int64_t parse_amount_cents(const char *str) {
    if (str[0] == '\0') return -1;

    // Find decimal point
    const char *dot = strchr(str, '.');
    int64_t whole = 0;
    int64_t frac = 0;

    if (dot) {
        // Parse whole part
        for (const char *p = str; p < dot; p++) {
            if (!isdigit((unsigned char)*p)) return -1;
            whole = whole * 10 + (*p - '0');
        }
        // Parse fractional part (up to 2 digits)
        int frac_digits = 0;
        for (const char *p = dot + 1; *p; p++) {
            if (!isdigit((unsigned char)*p)) return -1;
            if (frac_digits < 2) {
                frac = frac * 10 + (*p - '0');
                frac_digits++;
            } else {
                return -1; // too many decimal places
            }
        }
        // Pad to 2 decimal places
        if (frac_digits == 1) frac *= 10;
    } else {
        // No decimal point - whole number
        for (const char *p = str; *p; p++) {
            if (!isdigit((unsigned char)*p)) return -1;
            whole = whole * 10 + (*p - '0');
        }
    }

    return whole * 100 + frac;
}

static bool validate_date(const char *str) {
    if (strlen(str) != 10) return false;
    // Check format: YYYY-MM-DD
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (str[i] != '-') return false;
        } else {
            if (!isdigit((unsigned char)str[i])) return false;
        }
    }
    return true;
}

static bool form_validate_and_save(form_state_t *fs) {
    fs->error[0] = '\0';

    // Validate amount
    int64_t cents = parse_amount_cents(fs->amount);
    if (cents <= 0) {
        snprintf(fs->error, sizeof(fs->error), "Invalid amount");
        fs->current_field = FIELD_AMOUNT;
        return false;
    }

    // Validate date
    if (!validate_date(fs->date)) {
        snprintf(fs->error, sizeof(fs->error), "Invalid date (YYYY-MM-DD)");
        fs->current_field = FIELD_DATE;
        return false;
    }

    // Build transaction
    transaction_t txn = {0};
    txn.amount_cents = cents;
    txn.type = fs->txn_type;
    if (fs->account_count > 0)
        txn.account_id = fs->accounts[fs->account_sel].id;
    if (fs->category_count > 0)
        txn.category_id = fs->categories[fs->category_sel].id;
    snprintf(txn.date, sizeof(txn.date), "%s", fs->date);
    snprintf(txn.description, sizeof(txn.description), "%s", fs->desc);

    int64_t row_id = db_insert_transaction(fs->db, &txn);
    if (row_id < 0) {
        snprintf(fs->error, sizeof(fs->error), "Database error");
        return false;
    }

    return true;
}

static void handle_text_input(char *buf, int *pos, int maxlen, int ch,
                              bool digits_only) {
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
        if (digits_only) {
            if (ch == '.') {
                // Only allow one decimal point
                if (strchr(buf, '.')) return;
            } else if (!isdigit(ch)) {
                return;
            }
        }
        memmove(&buf[*pos + 1], &buf[*pos], len - *pos + 1);
        buf[*pos] = (char)ch;
        (*pos)++;
    }
}

static void handle_date_input(char *buf, int *pos, int ch) {
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
    } else if ((isdigit(ch) || ch == '-') && len < 10) {
        memmove(&buf[*pos + 1], &buf[*pos], len - *pos + 1);
        buf[*pos] = (char)ch;
        (*pos)++;
    }
}

form_result_t form_add_transaction(sqlite3 *db, WINDOW *parent) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    // Check minimum size
    if (ph < FORM_HEIGHT || pw < FORM_WIDTH) {
        return FORM_CANCELLED;
    }

    form_state_t fs;
    form_init_state(&fs, db);

    // Center the form over the parent content window
    int start_y, start_x;
    getbegyx(parent, start_y, start_x);
    int form_y = start_y + (ph - FORM_HEIGHT) / 2;
    int form_x = start_x + (pw - FORM_WIDTH) / 2;

    fs.win = newwin(FORM_HEIGHT, FORM_WIDTH, form_y, form_x);
    keypad(fs.win, TRUE);
    curs_set(1);

    form_result_t result = FORM_CANCELLED;
    bool done = false;

    while (!done) {
        form_draw(&fs);
        if (fs.dropdown_open)
            form_draw_dropdown(&fs);

        int ch = wgetch(fs.win);
        fs.error[0] = '\0';

        if (fs.dropdown_open) {
            int count = (fs.current_field == FIELD_ACCOUNT)
                ? fs.account_count : fs.category_count;
            switch (ch) {
            case KEY_UP:
                if (fs.dropdown_sel > 0) fs.dropdown_sel--;
                break;
            case KEY_DOWN:
                if (fs.dropdown_sel < count - 1) fs.dropdown_sel++;
                break;
            case '\n':
                form_close_dropdown(&fs, true);
                break;
            case 27: // Escape
                form_close_dropdown(&fs, false);
                break;
            }
            continue;
        }

        switch (ch) {
        case 27: // Escape
            done = true;
            break;

        case 19: // Ctrl+S
            if (form_validate_and_save(&fs)) {
                result = FORM_SAVED;
                done = true;
            }
            break;

        case '\t':
        case KEY_DOWN:
            if (fs.current_field < FIELD_COUNT - 1)
                fs.current_field++;
            break;

        case KEY_BTAB:
        case KEY_UP:
            if (fs.current_field > 0)
                fs.current_field--;
            break;

        case '\n':
            if (fs.current_field == FIELD_SUBMIT) {
                if (form_validate_and_save(&fs)) {
                    result = FORM_SAVED;
                    done = true;
                }
            } else if (fs.current_field == FIELD_ACCOUNT ||
                       fs.current_field == FIELD_CATEGORY) {
                form_open_dropdown(&fs);
            }
            break;

        case ' ':
            if (fs.current_field == FIELD_SUBMIT) {
                if (form_validate_and_save(&fs)) {
                    result = FORM_SAVED;
                    done = true;
                }
            } else if (fs.current_field == FIELD_TYPE) {
                fs.txn_type = (fs.txn_type == TRANSACTION_EXPENSE)
                    ? TRANSACTION_INCOME : TRANSACTION_EXPENSE;
                form_load_categories(&fs);
            } else if (fs.current_field == FIELD_ACCOUNT ||
                       fs.current_field == FIELD_CATEGORY) {
                form_open_dropdown(&fs);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos,
                                  (int)sizeof(fs.desc), ch, false);
            }
            break;

        case KEY_LEFT:
            if (fs.current_field == FIELD_TYPE) {
                fs.txn_type = TRANSACTION_EXPENSE;
                form_load_categories(&fs);
            } else if (fs.current_field == FIELD_AMOUNT) {
                handle_text_input(fs.amount, &fs.amount_pos,
                                  (int)sizeof(fs.amount), ch, true);
            } else if (fs.current_field == FIELD_DATE) {
                handle_date_input(fs.date, &fs.date_pos, ch);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos,
                                  (int)sizeof(fs.desc), ch, false);
            }
            break;

        case KEY_RIGHT:
            if (fs.current_field == FIELD_TYPE) {
                fs.txn_type = TRANSACTION_INCOME;
                form_load_categories(&fs);
            } else if (fs.current_field == FIELD_AMOUNT) {
                handle_text_input(fs.amount, &fs.amount_pos,
                                  (int)sizeof(fs.amount), ch, true);
            } else if (fs.current_field == FIELD_DATE) {
                handle_date_input(fs.date, &fs.date_pos, ch);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos,
                                  (int)sizeof(fs.desc), ch, false);
            }
            break;

        case KEY_RESIZE:
            done = true;
            break;

        default:
            // Text input for text fields
            if (fs.current_field == FIELD_AMOUNT) {
                handle_text_input(fs.amount, &fs.amount_pos,
                                  (int)sizeof(fs.amount), ch, true);
            } else if (fs.current_field == FIELD_DATE) {
                handle_date_input(fs.date, &fs.date_pos, ch);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos,
                                  (int)sizeof(fs.desc), ch, false);
            }
            break;
        }
    }

    curs_set(0);
    form_cleanup_state(&fs);
    return result;
}
