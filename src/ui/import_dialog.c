#include "ui/import_dialog.h"
#include "csv/csv_import.h"
#include "db/query.h"
#include "models/account.h"
#include "ui/colors.h"
#include "ui/resize.h"

#include <ctype.h>
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

typedef enum {
    IMPORT_CATEGORY_CREATE = 0,
    IMPORT_CATEGORY_ASSIGN,
    IMPORT_CATEGORY_LEAVE_UNCATEGORIZED
} import_category_action_t;

typedef struct {
    transaction_type_t txn_type;
    char normalized_category[64];
    int64_t category_id;
} category_resolution_t;

static bool names_equivalent(const char *a, const char *b) {
    if (!a || !b)
        return false;

    char na[96];
    char nb[96];
    int ia = 0;
    int ib = 0;
    for (int i = 0; a[i] && ia < (int)sizeof(na) - 1; i++) {
        if (isalnum((unsigned char)a[i]))
            na[ia++] = (char)tolower((unsigned char)a[i]);
    }
    for (int i = 0; b[i] && ib < (int)sizeof(nb) - 1; i++) {
        if (isalnum((unsigned char)b[i]))
            nb[ib++] = (char)tolower((unsigned char)b[i]);
    }
    na[ia] = '\0';
    nb[ib] = '\0';

    if (strcmp(na, nb) == 0)
        return true;

    const char *suffixes[] = {"cc", "card", "creditcard"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        const char *suffix = suffixes[i];
        size_t sfx_len = strlen(suffix);
        size_t na_len = strlen(na);
        size_t nb_len = strlen(nb);

        if (nb_len == na_len + sfx_len &&
            strncmp(nb, na, na_len) == 0 &&
            strcmp(nb + na_len, suffix) == 0) {
            return true;
        }
        if (na_len == nb_len + sfx_len &&
            strncmp(na, nb, nb_len) == 0 &&
            strcmp(na + nb_len, suffix) == 0) {
            return true;
        }
    }

    return false;
}

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

static void normalize_category_key(const char *src, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src)
        return;

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", src);
    trim_whitespace_in_place(tmp);

    size_t di = 0;
    for (size_t i = 0; tmp[i] != '\0' && di + 1 < dst_sz; i++)
        dst[di++] = (char)tolower((unsigned char)tmp[i]);
    dst[di] = '\0';
}

static bool category_names_equivalent(const char *a, const char *b) {
    char na[64];
    char nb[64];
    normalize_category_key(a, na, sizeof(na));
    normalize_category_key(b, nb, sizeof(nb));
    return na[0] != '\0' && strcmp(na, nb) == 0;
}

static category_type_t category_type_for_transaction(transaction_type_t type) {
    return type == TRANSACTION_INCOME ? CATEGORY_INCOME : CATEGORY_EXPENSE;
}

static int64_t find_category_id_by_name(const category_t *categories,
                                        int category_count,
                                        const char *name) {
    if (!categories || category_count <= 0 || !name || name[0] == '\0')
        return 0;
    for (int i = 0; i < category_count; i++) {
        if (category_names_equivalent(categories[i].name, name))
            return categories[i].id;
    }
    return 0;
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

static int64_t create_category_from_import_label(sqlite3 *db,
                                                 category_type_t ctype,
                                                 const char *label) {
    if (!db || !label || label[0] == '\0')
        return -1;

    char trimmed[64];
    snprintf(trimmed, sizeof(trimmed), "%s", label);
    trim_whitespace_in_place(trimmed);
    if (trimmed[0] == '\0')
        return -1;

    char parent_name[64];
    char child_name[64];
    bool has_parent = false;
    if (!parse_category_path(trimmed, parent_name, sizeof(parent_name), child_name,
                             sizeof(child_name), &has_parent)) {
        return db_get_or_create_category(db, ctype, trimmed, 0);
    }

    if (!has_parent)
        return db_get_or_create_category(db, ctype, child_name, 0);

    int64_t parent_id = db_get_or_create_category(db, ctype, parent_name, 0);
    if (parent_id <= 0)
        return -1;
    return db_get_or_create_category(db, ctype, child_name, parent_id);
}

static int prompt_unknown_category_action(WINDOW *parent, const char *source_name,
                                          transaction_type_t txn_type,
                                          import_category_action_t *out_action) {
    if (!parent || !source_name || !out_action)
        return 0;

    const char *options[] = {"Create imported category", "Assign to existing category",
                             "Leave uncategorized"};
    const int option_count = 3;

    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = 11;
    int win_w = 72;
    if (ph < win_h)
        win_h = ph;
    if (pw < win_w)
        win_w = pw;
    if (win_h < 9 || win_w < 44)
        return 0;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    if (!w)
        return 0;
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));

    int sel = 0;
    bool done = false;
    bool confirmed = false;

    while (!done) {
        werase(w);
        box(w, 0, 0);
        mvwprintw(w, 1, 2, "Unmapped import category: %.52s", source_name);
        mvwprintw(w, 2, 2, "Transaction type: %s",
                  txn_type == TRANSACTION_INCOME ? "Income" : "Expense");

        for (int i = 0; i < option_count; i++) {
            int row = 4 + i;
            if (i == sel)
                wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
            mvwprintw(w, row, 2, "%-*s", win_w - 4, "");
            mvwprintw(w, row, 2, "%.*s", win_w - 4, options[i]);
            if (i == sel)
                wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        }

        mvwprintw(w, win_h - 2, 2, "Enter:Choose  Esc:Cancel import  ↑↓ move");
        wrefresh(w);

        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            done = true;
            break;
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
        case KEY_ENTER:
            *out_action = (import_category_action_t)sel;
            confirmed = true;
            done = true;
            break;
        case 27:
            done = true;
            break;
        default:
            break;
        }
    }

    delwin(w);
    touchwin(parent);
    return confirmed ? 1 : 0;
}

static int prompt_assign_existing_category(WINDOW *parent,
                                           const category_t *categories,
                                           int category_count,
                                           int64_t *out_category_id) {
    if (!parent || !categories || category_count <= 0 || !out_category_id)
        return 0;

    int ph, pw;
    getmaxyx(parent, ph, pw);

    int visible = category_count < 7 ? category_count : 7;
    int win_h = visible + 7;
    int win_w = 68;
    if (ph < win_h)
        win_h = ph;
    if (pw < win_w)
        win_w = pw;
    if (win_h < 9 || win_w < 42)
        return 0;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    if (!w)
        return 0;
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));

    int sel = 0;
    int scroll = 0;
    bool confirmed = false;
    bool done = false;

    while (!done) {
        if (sel < scroll)
            scroll = sel;
        if (sel >= scroll + visible)
            scroll = sel - visible + 1;

        werase(w);
        box(w, 0, 0);
        mvwprintw(w, 1, 2, "Assign imported category to:");

        int list_w = win_w - 4;
        for (int i = 0; i < visible; i++) {
            int idx = scroll + i;
            if (idx >= category_count)
                break;
            int row = 3 + i;
            if (idx == sel)
                wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
            mvwprintw(w, row, 2, "%-*s", list_w, "");
            mvwprintw(w, row, 2, "%.*s", list_w, categories[idx].name);
            if (idx == sel)
                wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
        }
        if (scroll > 0)
            mvwaddch(w, 3, win_w - 2, ACS_UARROW);
        if (scroll + visible < category_count)
            mvwaddch(w, 3 + visible - 1, win_w - 2, ACS_DARROW);

        mvwprintw(w, win_h - 2, 2, "Enter:Choose  Esc:Back  ↑↓ move");
        wrefresh(w);

        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            done = true;
            break;
        }
        switch (ch) {
        case KEY_UP:
        case 'k':
            if (sel > 0)
                sel--;
            break;
        case KEY_DOWN:
        case 'j':
            if (sel < category_count - 1)
                sel++;
            break;
        case '\n':
        case KEY_ENTER:
            *out_category_id = categories[sel].id;
            confirmed = true;
            done = true;
            break;
        case 27:
            done = true;
            break;
        default:
            break;
        }
    }

    delwin(w);
    touchwin(parent);
    return confirmed ? 1 : 0;
}

static int apply_import_categories(WINDOW *parent, sqlite3 *db,
                                   csv_parse_result_t *parse_result,
                                   char *error, size_t error_sz) {
    if (!parent || !db || !parse_result)
        return -1;

    category_t *expense_categories = NULL;
    category_t *income_categories = NULL;
    int expense_count = db_get_categories(db, CATEGORY_EXPENSE, &expense_categories);
    int income_count = db_get_categories(db, CATEGORY_INCOME, &income_categories);
    if (expense_count < 0 || income_count < 0) {
        free(expense_categories);
        free(income_categories);
        snprintf(error, error_sz, "Error loading categories.");
        return -1;
    }

    int max_resolutions = parse_result->row_count > 0 ? parse_result->row_count : 1;
    category_resolution_t *resolutions =
        calloc((size_t)max_resolutions, sizeof(*resolutions));
    if (!resolutions) {
        free(expense_categories);
        free(income_categories);
        snprintf(error, error_sz, "Out of memory.");
        return -1;
    }

    int resolution_count = 0;
    bool prompt_unknown = (parse_result->type == CSV_TYPE_QIF);

    for (int i = 0; i < parse_result->row_count; i++) {
        csv_row_t *row = &parse_result->rows[i];
        if (!row->has_category)
            continue;

        trim_whitespace_in_place(row->category);
        if (row->category[0] == '\0') {
            row->has_category = false;
            continue;
        }

        char normalized[64];
        normalize_category_key(row->category, normalized, sizeof(normalized));
        if (normalized[0] == '\0') {
            row->has_category = false;
            continue;
        }

        bool cached = false;
        for (int ri = 0; ri < resolution_count; ri++) {
            if (resolutions[ri].txn_type == row->type &&
                strcmp(resolutions[ri].normalized_category, normalized) == 0) {
                row->category_id = resolutions[ri].category_id;
                cached = true;
                break;
            }
        }
        if (cached)
            continue;

        category_t *typed_categories =
            (row->type == TRANSACTION_INCOME) ? income_categories : expense_categories;
        int typed_count =
            (row->type == TRANSACTION_INCOME) ? income_count : expense_count;

        int64_t resolved_id =
            find_category_id_by_name(typed_categories, typed_count, row->category);

        if (resolved_id == 0 && prompt_unknown) {
            while (true) {
                import_category_action_t action = IMPORT_CATEGORY_LEAVE_UNCATEGORIZED;
                if (!prompt_unknown_category_action(parent, row->category, row->type,
                                                    &action)) {
                    snprintf(error, error_sz, "Import canceled.");
                    free(resolutions);
                    free(expense_categories);
                    free(income_categories);
                    return 0;
                }

                if (action == IMPORT_CATEGORY_LEAVE_UNCATEGORIZED) {
                    resolved_id = 0;
                    break;
                }

                if (action == IMPORT_CATEGORY_ASSIGN) {
                    int64_t selected_id = 0;
                    if (prompt_assign_existing_category(parent, typed_categories,
                                                        typed_count, &selected_id)) {
                        resolved_id = selected_id;
                        break;
                    }
                    continue;
                }

                category_type_t ctype = category_type_for_transaction(row->type);
                int64_t created_id =
                    create_category_from_import_label(db, ctype, row->category);
                if (created_id <= 0) {
                    snprintf(error, error_sz, "Error creating category.");
                    free(resolutions);
                    free(expense_categories);
                    free(income_categories);
                    return -1;
                }
                resolved_id = created_id;

                if (ctype == CATEGORY_EXPENSE) {
                    free(expense_categories);
                    expense_categories = NULL;
                    expense_count =
                        db_get_categories(db, CATEGORY_EXPENSE, &expense_categories);
                    if (expense_count < 0) {
                        snprintf(error, error_sz, "Error loading categories.");
                        free(resolutions);
                        free(expense_categories);
                        free(income_categories);
                        return -1;
                    }
                } else {
                    free(income_categories);
                    income_categories = NULL;
                    income_count =
                        db_get_categories(db, CATEGORY_INCOME, &income_categories);
                    if (income_count < 0) {
                        snprintf(error, error_sz, "Error loading categories.");
                        free(resolutions);
                        free(expense_categories);
                        free(income_categories);
                        return -1;
                    }
                }
                break;
            }
        }

        row->category_id = resolved_id;
        if (resolution_count < max_resolutions) {
            resolutions[resolution_count].txn_type = row->type;
            snprintf(resolutions[resolution_count].normalized_category,
                     sizeof(resolutions[resolution_count].normalized_category), "%s",
                     normalized);
            resolutions[resolution_count].category_id = resolved_id;
            resolution_count++;
        }
    }

    free(resolutions);
    free(expense_categories);
    free(income_categories);
    return 1;
}

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
            draw_border(w, win_h, win_w, " Import File ",
                        " Enter:Import  Esc:Cancel ");

            mvwprintw(w, 2, 2, "File path (.csv/.qif):");

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

                int category_rc = apply_import_categories(
                    parent, db, &parse_result, path_error, sizeof(path_error));
                if (category_rc <= 0) {
                    csv_parse_result_free(&parse_result);
                    if (path_error[0] == '\0')
                        snprintf(path_error, sizeof(path_error),
                                 "Could not resolve import categories.");
                    break;
                }

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

                    // Pre-select QIF account name when available; otherwise use
                    // current account if possible.
                    acct_sel = 0;
                    bool preselected = false;
                    if (parse_result.type == CSV_TYPE_QIF &&
                        parse_result.source_account[0]) {
                        for (int i = 0; i < account_count; i++) {
                            if (names_equivalent(accounts[i].name,
                                                 parse_result.source_account)) {
                                acct_sel = i;
                                preselected = true;
                                break;
                            }
                        }
                    }
                    if (!preselected) {
                        for (int i = 0; i < account_count; i++) {
                            if (accounts[i].id == current_account_id) {
                                acct_sel = i;
                                break;
                            }
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

            draw_border(w, win_h, win_w, " Import File \u2013 Credit Card ",
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
        // STAGE_SELECT_ACCT: scrollable account list for account-targeted import
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
                        " Import File \u2013 Select Account ",
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
            draw_border(w, win_h, win_w, " Import File ",
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
