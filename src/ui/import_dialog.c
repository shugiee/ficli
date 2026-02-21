#include "ui/import_dialog.h"
#include "csv/csv_import.h"
#include "db/query.h"
#include "models/account.h"
#include "ui/colors.h"
#include "ui/resize.h"

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 56
#define WIN_H 20

typedef enum {
    STAGE_PATH,
    STAGE_CONFIRM_CC,
    STAGE_SELECT_ACCT,
    STAGE_RESULT,
    STAGE_ERROR,
} dialog_stage_t;

// Info about one distinct card found in a CC import result.
typedef struct {
    char last4[5];
    int64_t account_id;     // 0 = no matching account
    char account_name[64];
    int txn_count;
    int dup_count;          // of txn_count, how many already exist in DB
} card_entry_t;

// Build a deduplicated list of cards from the parse result and match them
// against CREDIT_CARD accounts. Returns count of cards, fills *out (caller frees).
static int build_card_entries(const csv_parse_result_t *r, sqlite3 *db,
                              card_entry_t **out) {
    *out = NULL;

    account_t *accounts = NULL;
    int account_count = db_get_accounts(db, &accounts);
    if (account_count < 0)
        account_count = 0;

    int capacity = 8;
    card_entry_t *cards = malloc(capacity * sizeof(card_entry_t));
    if (!cards) {
        free(accounts);
        return 0;
    }
    int count = 0;

    for (int i = 0; i < r->row_count; i++) {
        const char *l4 = r->rows[i].card_last4;
        if (l4[0] == '\0')
            continue;

        // Check if we already have this card
        bool found = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(cards[j].last4, l4) == 0) {
                cards[j].txn_count++;
                found = true;
                break;
            }
        }
        if (found)
            continue;

        if (count >= capacity) {
            capacity *= 2;
            card_entry_t *tmp = realloc(cards, capacity * sizeof(card_entry_t));
            if (!tmp) {
                free(cards);
                free(accounts);
                return 0;
            }
            cards = tmp;
        }

        card_entry_t *ce = &cards[count++];
        memset(ce, 0, sizeof(*ce));
        snprintf(ce->last4, sizeof(ce->last4), "%s", l4);
        ce->txn_count = 1;

        // Match to a CREDIT_CARD account
        for (int j = 0; j < account_count; j++) {
            if (accounts[j].type == ACCOUNT_CREDIT_CARD &&
                strcmp(accounts[j].card_last4, l4) == 0) {
                ce->account_id = accounts[j].id;
                snprintf(ce->account_name, sizeof(ce->account_name), "%s",
                         accounts[j].name);
                break;
            }
        }
    }

    // Compute dup_count for each matched card by checking existing DB transactions.
    for (int ci = 0; ci < count; ci++) {
        card_entry_t *ce = &cards[ci];
        if (ce->account_id == 0)
            continue;

        txn_row_t *existing = NULL;
        int nexisting = db_get_transactions(db, ce->account_id, &existing);
        if (nexisting <= 0) {
            free(existing);
            continue;
        }

        bool *consumed = calloc(nexisting, sizeof(bool));
        if (!consumed) {
            free(existing);
            continue;
        }

        for (int i = 0; i < r->row_count; i++) {
            const csv_row_t *row = &r->rows[i];
            if (strcmp(row->card_last4, ce->last4) != 0)
                continue;
            for (int j = 0; j < nexisting; j++) {
                if (!consumed[j] &&
                    existing[j].amount_cents == row->amount_cents &&
                    existing[j].type == row->type &&
                    strcmp(existing[j].date, row->date) == 0 &&
                    strcmp(existing[j].payee, row->payee) == 0) {
                    consumed[j] = true;
                    ce->dup_count++;
                    break;
                }
            }
        }

        free(consumed);
        free(existing);
    }

    free(accounts);
    *out = cards;
    return count;
}

static void draw_border(WINDOW *w, int win_h, int win_w, const char *title,
                        const char *footer) {
    werase(w);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    if (title) {
        int tlen = (int)strlen(title);
        int tx = (win_w - tlen) / 2;
        if (tx < 1)
            tx = 1;
        mvwprintw(w, 0, tx, "%s", title);
    }

    if (footer) {
        int flen = (int)strlen(footer);
        int fx = (win_w - flen) / 2;
        if (fx < 1)
            fx = 1;
        mvwprintw(w, win_h - 1, fx, "%s", footer);
    }
}

int import_dialog(WINDOW *parent, sqlite3 *db, int64_t current_account_id) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = WIN_H;
    int win_w = WIN_W;
    if (win_h > ph)
        win_h = ph;
    if (win_w > pw)
        win_w = pw;
    if (win_h < 8 || win_w < 30)
        return -1;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    keypad(w, TRUE);

    dialog_stage_t stage = STAGE_PATH;
    int result_count = 0;
    int result_skipped = 0;
    bool done = false;
    int ret = -1;

    // Path stage state
    char path_buf[1024];
    int path_len = 0;
    int path_view = 0;
    char path_error[256];
    path_buf[0] = '\0';
    path_error[0] = '\0';

    // Field width for path input (inside border with padding)
    int field_x = 2;
    int field_w = win_w - 4;

    // CC confirm state
    csv_parse_result_t parse_result = {0};
    card_entry_t *cards = NULL;
    int card_count = 0;

    // Account select state
    account_t *accounts = NULL;
    int account_count = 0;
    int acct_sel = 0;
    int acct_scroll = 0;

    while (!done) {
        switch (stage) {

        // ----------------------------------------------------------------
        // STAGE_PATH: text input for file path
        // ----------------------------------------------------------------
        case STAGE_PATH: {
            curs_set(1);
            draw_border(w, win_h, win_w, " Import CSV ",
                        " Enter:Import  Esc:Cancel ");

            mvwprintw(w, 2, 2, "File path:");

            // Path input field (highlight with COLOR_FORM_ACTIVE)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
            mvwprintw(w, 3, field_x, "%-*s", field_w, "");
            // Show visible portion of path
            const char *vis = path_buf + path_view;
            int vis_len = path_len - path_view;
            if (vis_len < 0)
                vis_len = 0;
            if (vis_len > field_w)
                vis_len = field_w;
            mvwprintw(w, 3, field_x, "%.*s", vis_len, vis);
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

            // Error message
            if (path_error[0]) {
                wattron(w, A_BOLD);
                mvwprintw(w, 5, 2, "%-*.*s", win_w - 4, win_w - 4, path_error);
                wattroff(w, A_BOLD);
            }

            // Position cursor at end of visible text
            int cursor_col = field_x + (path_len - path_view);
            if (cursor_col > field_x + field_w - 1)
                cursor_col = field_x + field_w - 1;
            wmove(w, 3, cursor_col);
            wrefresh(w);

            int ch = wgetch(w);
            if (ui_requeue_resize_event(ch)) {
                done = true;
                ret = -1;
                break;
            }
            if (ch == 27) {
                done = true;
                ret = -1;
                break;
            }
            if (ch == '\n' || ch == KEY_ENTER) {
                if (path_len == 0) {
                    snprintf(path_error, sizeof(path_error), "Please enter a file path.");
                    break;
                }
                // Try to parse
                csv_parse_result_free(&parse_result);
                parse_result = csv_parse_file(path_buf);
                if (parse_result.error[0] || parse_result.row_count == 0) {
                    if (parse_result.error[0])
                        snprintf(path_error, sizeof(path_error), "%s",
                                 parse_result.error);
                    else
                        snprintf(path_error, sizeof(path_error),
                                 "No transactions found.");
                    csv_parse_result_free(&parse_result);
                    break;
                }
                path_error[0] = '\0';

                if (parse_result.type == CSV_TYPE_CREDIT_CARD) {
                    free(cards);
                    card_count = build_card_entries(&parse_result, db, &cards);
                    stage = STAGE_CONFIRM_CC;
                } else {
                    // Load accounts for selection
                    free(accounts);
                    accounts = NULL;
                    account_count = db_get_accounts(db, &accounts);
                    if (account_count < 0)
                        account_count = 0;

                    // Pre-select current account if possible
                    acct_sel = 0;
                    for (int i = 0; i < account_count; i++) {
                        if (accounts[i].id == current_account_id) {
                            acct_sel = i;
                            break;
                        }
                    }
                    acct_scroll = 0;
                    stage = STAGE_SELECT_ACCT;
                }
                break;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (path_len > 0) {
                    path_buf[--path_len] = '\0';
                    // Adjust view
                    if (path_len - path_view < 0)
                        path_view = path_len;
                }
                break;
            }
            if (ch >= 32 && ch < 127) {
                if (path_len < (int)sizeof(path_buf) - 1) {
                    path_buf[path_len++] = (char)ch;
                    path_buf[path_len] = '\0';
                    // Scroll view right if needed
                    if (path_len - path_view >= field_w)
                        path_view = path_len - field_w + 1;
                }
                break;
            }
            break;
        }

        // ----------------------------------------------------------------
        // STAGE_CONFIRM_CC: show card list and import/skip counts
        // ----------------------------------------------------------------
        case STAGE_CONFIRM_CC: {
            curs_set(0);

            // Calculate import/skip counts
            int will_import = 0, will_dupes = 0, will_unmatched = 0;
            for (int i = 0; i < card_count; i++) {
                if (cards[i].account_id) {
                    will_import += cards[i].txn_count - cards[i].dup_count;
                    will_dupes += cards[i].dup_count;
                } else {
                    will_unmatched += cards[i].txn_count;
                }
            }

            draw_border(w, win_h, win_w, " Import CSV \u2013 Credit Card ",
                        " Enter:Import  Esc:Cancel ");

            int row = 2;
            wattron(w, A_BOLD);
            mvwprintw(w, row, 2, "%-4s  %-*s", "Card",
                      win_w - 10, "Account");
            wattroff(w, A_BOLD);
            row++;
            mvwprintw(w, row, 2, "----  %-*s", win_w - 10, "-------");
            row++;

            for (int i = 0; i < card_count && row < win_h - 3; i++) {
                const char *acct_name = cards[i].account_id
                    ? cards[i].account_name
                    : "(no matching account)";
                mvwprintw(w, row++, 2, "%-4s  %-*.*s", cards[i].last4,
                          win_w - 10, win_w - 10, acct_name);
            }

            row++;
            if (row < win_h - 1) {
                mvwprintw(w, row, 2, "Import: %d  Dupes: %d  No acct: %d",
                          will_import, will_dupes, will_unmatched);
            }

            wrefresh(w);

            int ch = wgetch(w);
            if (ui_requeue_resize_event(ch)) {
                done = true;
                ret = -1;
                break;
            }
            if (ch == 27) {
                done = true;
                ret = -1;
            } else if (ch == '\n' || ch == KEY_ENTER) {
                int imp = 0, skp = 0;
                int rc = csv_import_credit_card(db, &parse_result, &imp, &skp);
                if (rc < 0) {
                    snprintf(path_error, sizeof(path_error),
                             "Database error during import.");
                    stage = STAGE_ERROR;
                } else {
                    result_count = imp;
                    result_skipped = skp;
                    ret = imp;
                    stage = STAGE_RESULT;
                }
            }
            break;
        }

        // ----------------------------------------------------------------
        // STAGE_SELECT_ACCT: scrollable account list for CS import
        // ----------------------------------------------------------------
        case STAGE_SELECT_ACCT: {
            curs_set(0);

            int list_h = win_h - 4; // rows available for list (inside border + top/bottom)
            if (list_h < 1)
                list_h = 1;

            // Clamp selection and scroll
            if (acct_sel >= account_count && account_count > 0)
                acct_sel = account_count - 1;
            if (acct_sel < acct_scroll)
                acct_scroll = acct_sel;
            if (acct_sel >= acct_scroll + list_h)
                acct_scroll = acct_sel - list_h + 1;

            draw_border(w, win_h, win_w,
                        " Import CSV \u2013 Select Account ",
                        " Enter:Import  j/k:Navigate  Esc:Cancel ");

            for (int i = 0; i < list_h; i++) {
                int idx = acct_scroll + i;
                if (idx >= account_count)
                    break;
                int list_row = 2 + i;

                if (idx == acct_sel) {
                    wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
                    mvwprintw(w, list_row, 2, " %-*.*s", win_w - 5, win_w - 5,
                              accounts[idx].name);
                    wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
                } else {
                    mvwprintw(w, list_row, 2, " %-*.*s", win_w - 5, win_w - 5,
                              accounts[idx].name);
                }
            }

            // Scroll indicators
            if (acct_scroll > 0)
                mvwprintw(w, 2, win_w - 2, "\u25b2");
            if (acct_scroll + list_h < account_count)
                mvwprintw(w, win_h - 2, win_w - 2, "\u25bc");

            wrefresh(w);

            int ch = wgetch(w);
            if (ui_requeue_resize_event(ch)) {
                done = true;
                ret = -1;
                break;
            }
            if (ch == 27) {
                done = true;
                ret = -1;
            } else if (ch == '\n' || ch == KEY_ENTER) {
                if (account_count > 0) {
                    int imp = 0, skp = 0;
                    int rc = csv_import_checking(db, &parse_result,
                                                 accounts[acct_sel].id, &imp, &skp);
                    if (rc < 0) {
                        snprintf(path_error, sizeof(path_error),
                                 "Database error during import.");
                        stage = STAGE_ERROR;
                    } else {
                        result_count = imp;
                        result_skipped = skp;
                        ret = imp;
                        stage = STAGE_RESULT;
                    }
                }
            } else if (ch == KEY_UP || ch == 'k') {
                if (acct_sel > 0)
                    acct_sel--;
            } else if (ch == KEY_DOWN || ch == 'j') {
                if (acct_sel < account_count - 1)
                    acct_sel++;
            }
            break;
        }

        // ----------------------------------------------------------------
        // STAGE_RESULT: show final counts
        // ----------------------------------------------------------------
        case STAGE_RESULT: {
            curs_set(0);
            draw_border(w, win_h, win_w, " Import CSV ",
                        " Any key to close ");

            mvwprintw(w, win_h / 2 - 1, 2, "Imported: %d   Skipped: %d",
                      result_count, result_skipped);

            wrefresh(w);
            int ch = wgetch(w);
            (void)ui_requeue_resize_event(ch);
            done = true;
            break;
        }

        // ----------------------------------------------------------------
        // STAGE_ERROR: show error message
        // ----------------------------------------------------------------
        case STAGE_ERROR: {
            curs_set(0);
            draw_border(w, win_h, win_w, " Import Error ",
                        " Any key to close ");

            wattron(w, A_BOLD);
            mvwprintw(w, win_h / 2 - 1, 2, "%-*.*s", win_w - 4, win_w - 4,
                      path_error);
            wattroff(w, A_BOLD);

            wrefresh(w);
            int ch = wgetch(w);
            (void)ui_requeue_resize_event(ch);
            done = true;
            ret = -1;
            break;
        }
        }
    }

    curs_set(0);
    csv_parse_result_free(&parse_result);
    free(cards);
    free(accounts);
    delwin(w);
    touchwin(parent);
    return ret;
}
